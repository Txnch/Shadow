#include "search.h"
#include "movegen.h"
#include "movepick.h"
#include "evaluate.h"
#include "move.h"
#include "timeman.h"
#include "tt.h"
#include "wdl.h"

#include <iostream>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <memory>

bool g_silent_search = false;
std::atomic<bool> g_uci_show_wdl(false);

inline constexpr int INF = SEARCH_INF;
inline constexpr int MATE_SCORE = SEARCH_MATE_SCORE;
inline constexpr int MAX_EVAL_SCORE = MATE_SCORE - 1000;

inline constexpr int HISTORY_CLAMP = 16384;
inline constexpr int STAT_BONUS_MAX = 1409;
inline constexpr int STAT_BONUS_MULT = 175;
inline constexpr int STAT_BONUS_BASE = 15;
inline constexpr int STAT_MALUS_MAX = 1047;
inline constexpr int STAT_MALUS_MULT = 196;
inline constexpr int STAT_MALUS_BASE = 25;

inline constexpr int ROOT_ASPIRATION_DEPTH = 3;
inline constexpr int ROOT_ASPIRATION_DELTA_BASE = 16;
inline constexpr int ROOT_ASPIRATION_WIDENING_FACTOR = 17;
inline constexpr int ROOT_ASPIRATION_REDUCTION_MAX = 3;
inline constexpr int QS_MAX_PLY_GUARD = MAX_PLY - 4;
inline constexpr int SINGULAR_BETA_MARGIN = 64;
inline constexpr int SINGULAR_DOUBLE_EXT_MARGIN = 13;
inline constexpr int SINGULAR_TRIPLE_EXT_MARGIN = 121;

enum NodeType {
    Root,
    PV,
    NonPV
};

// LMR
inline constexpr int LMR_SCALE = 1024;
inline constexpr int LMR_CHECK_REDUCTION = 851;
inline constexpr uint64_t SEARCH_POLL_MASK = 4095ULL;

static int LMR_TABLE[2][64][256];


static int init_lmr_table = []() {
    for (int d = 1; d < 64; ++d) {
        for (int m = 1; m < 256; ++m) {
            double base = std::log(d) * std::log(m) / 2.25;
            LMR_TABLE[0][d][m] = static_cast<int>((0.40 + base) * LMR_SCALE);
            LMR_TABLE[1][d][m] = static_cast<int>((0.80 + base) * LMR_SCALE);
        }
    }
    return 0;
    }();

static std::atomic<uint64_t> global_stop_epoch(0);
static thread_local SearchThreadState search_state;
static thread_local TimeManager tm;
static thread_local std::unique_ptr<SearchMoveBuffer[]> search_move_buffers;

static inline bool external_stop_requested()
{
    return global_stop_epoch.load(std::memory_order_relaxed) != search_state.local_stop_epoch;
}

static inline bool search_stop_requested()
{
    return search_state.stop_flag || external_stop_requested();
}

static inline int draw_score()
{
    return -1 + (search_state.nodes_count & 2);
}

static inline int clamp_eval_score(int score)
{
    return std::clamp(score, -MAX_EVAL_SCORE, MAX_EVAL_SCORE);
}

static inline void update_history_gravity(int& hist, int bonus)
{
    hist += bonus - (hist * std::abs(bonus)) / HISTORY_CLAMP;
}

static inline void update_history_with_base(int& hist, int bonus, int base)
{
    hist += bonus - (base * std::abs(bonus)) / HISTORY_CLAMP;
}

static inline int stat_bonus(int depth)
{
    return std::min(STAT_BONUS_MULT * depth + STAT_BONUS_BASE, STAT_BONUS_MAX);
}

static inline int stat_malus(int depth)
{
    return std::min(STAT_MALUS_MULT * depth - STAT_MALUS_BASE, STAT_MALUS_MAX);
}

static bool is_shuffling(Move m, const SearchStack* ss, int ply, const Position& pos)
{
    if (is_capture(m) || is_promotion(m) || pos.halfmove_clock() < 10)
        return false;

    if (ply < 20)
        return false;

    const Move prev_same_side = ss[ply - 2].current_move;
    const Move older_same_side = ss[ply - 4].current_move;
    if (!prev_same_side || !older_same_side)
        return false;

    return from_sq(m) == to_sq(prev_same_side)
        && from_sq(prev_same_side) == to_sq(older_same_side);
}

struct SearchMoveBufferScope {
    SearchMoveBuffer* buffer = nullptr;

    SearchMoveBufferScope()
    {
        if (!search_move_buffers)
            search_move_buffers = std::make_unique<SearchMoveBuffer[]>(SEARCH_MOVE_BUFFER_COUNT);

        const int index = search_state.move_buffer_depth++;
        buffer = &search_move_buffers[std::min(index, SEARCH_MOVE_BUFFER_COUNT - 1)];
    }

    ~SearchMoveBufferScope()
    {
        --search_state.move_buffer_depth;
    }
};

// Eval Correction Helpers
static int get_eval_correction(const Position& pos, const SearchStack* ss, int ply)
{
    int stm = pos.side_to_move();
    int score = 0;

    score += search_state.pawn_corr[stm][pos.pawn_key() & CORR_MASK];
    score += search_state.non_pawn_corr[stm][WHITE][pos.non_pawn_key(WHITE) & CORR_MASK];
    score += search_state.non_pawn_corr[stm][BLACK][pos.non_pawn_key(BLACK) & CORR_MASK];

    if (ply >= 1 && ss[ply - 1].current_move != 0) {
        Piece prev_piece = ss[ply - 1].moved_piece;
        Square prev_to = to_sq(ss[ply - 1].current_move);
        score += search_state.cont_corr[prev_piece][prev_to];
    }

    return score / 128;
}

static void update_eval_correction(const Position& pos, const SearchStack* ss, int ply, int depth, int diff)
{
    int stm = pos.side_to_move();

    int bonus = std::clamp((diff * depth) / 8, -CORR_BONUS_LIMIT, CORR_BONUS_LIMIT);

    auto update_weight = [](int& entry, int b) {
        entry += b - (entry * std::abs(b)) / CORR_HISTORY_LIMIT;
        entry = std::clamp(entry, -CORR_HISTORY_LIMIT, CORR_HISTORY_LIMIT);
        };

    update_weight(search_state.pawn_corr[stm][pos.pawn_key() & CORR_MASK], bonus);
    update_weight(search_state.non_pawn_corr[stm][WHITE][pos.non_pawn_key(WHITE) & CORR_MASK], bonus);
    update_weight(search_state.non_pawn_corr[stm][BLACK][pos.non_pawn_key(BLACK) & CORR_MASK], bonus);

    if (ply >= 1 && ss[ply - 1].current_move != 0) {
        Piece prev_piece = ss[ply - 1].moved_piece;
        Square prev_to = to_sq(ss[ply - 1].current_move);
        update_weight(search_state.cont_corr[prev_piece][prev_to], bonus);
    }
}

static void update_hard_time()
{
    if (search_stop_requested()) return;

    if (!search_state.target_nodes_soft && search_state.target_max_nodes > 0 && search_state.nodes_count >= search_state.target_max_nodes) {
        search_state.stop_flag = true;
        return;
    }

    if (tm.hard_stop())
        search_state.stop_flag = true;
}

void stop_search_now()
{
    global_stop_epoch.fetch_add(1, std::memory_order_seq_cst);
    search_state.stop_flag = true;
}

uint64_t current_stop_epoch()
{
    return global_stop_epoch.load(std::memory_order_relaxed);
}

void clear_search_state_for_new_game()
{
    search_state.stop_flag = false;
    search_state.nodes_count = 0;
    search_state.target_max_nodes = 0;
    search_state.target_nodes_soft = false;
    search_state.seldepth = 0;
    search_state.nmp_min_ply = 0;

    std::fill(&search_state.killer_moves[0][0], &search_state.killer_moves[0][0] + 2 * MAX_PLY, Move(0));
    std::fill(&search_state.history_heuristic[0][0], &search_state.history_heuristic[0][0] + 64 * 64, 0);
    std::fill(&search_state.countermove[0][0], &search_state.countermove[0][0] + 64 * 64, Move(0));

    std::fill(&search_state.cont_history[0][0][0][0],
        &search_state.cont_history[0][0][0][0] + PIECE_NB * 64 * PIECE_NB * 64, 0);

    std::fill(&search_state.capture_history[0][0][0], &search_state.capture_history[0][0][0] + 16 * 64 * 16, 0);

    std::fill(&search_state.pv_table[0][0], &search_state.pv_table[0][0] + MAX_PLY * MAX_PLY, Move(0));
    std::fill(search_state.pv_length, search_state.pv_length + MAX_PLY, 0);

    std::fill(&search_state.pawn_corr[0][0], &search_state.pawn_corr[0][0] + 2 * CORR_SIZE, 0);
    std::fill(&search_state.non_pawn_corr[0][0][0], &search_state.non_pawn_corr[0][0][0] + 4 * CORR_SIZE, 0);
    std::fill(&search_state.cont_corr[0][0], &search_state.cont_corr[0][0] + PIECE_NB * 64, 0);

    tm.reset();
}

static int total_pieces(const Position& pos)
{
    return popcount(pos.pieces(WHITE) | pos.pieces(BLACK));
}

static bool has_non_pawn_material(const Position& pos, Color c)
{
    return (pos.pieces(c) & ~(pos.pieces(PAWN) | pos.pieces(KING))) != 0;
}

// Insufficient Material
bool has_insufficient_material(const Position& pos)
{
    if (pos.pieces(PAWN) || pos.pieces(ROOK) || pos.pieces(QUEEN))
        return false;

    const int minors = popcount((pos.pieces(WHITE) | pos.pieces(BLACK)) & (pos.pieces(KNIGHT) | pos.pieces(BISHOP)));

    if (minors <= 1)
        return true;

    return false;
}

static inline Piece captured_piece_for_move(const Position& pos, Move m)
{
    if (!is_capture(m))
        return NO_PIECE;

    if (is_en_passant(m))
        return make_piece(~pos.side_to_move(), PAWN);

    return pos.piece_on(to_sq(m));
}

static inline int capture_history_score(const Position& pos, Move m)
{
    if (!is_capture(m) && !is_promotion(m))
        return 0;

    const Piece attacker = pos.piece_on(from_sq(m));
    Piece victim = NO_PIECE;
    if (is_capture(m)) {
        victim = captured_piece_for_move(pos, m);
    }

    if (attacker == NO_PIECE)
        return 0;

    return search_state.capture_history[attacker][to_sq(m)][victim];
}

static inline void update_capture_history(Piece attacker, Square to, Piece victim, int delta)
{
    if (attacker == NO_PIECE)
        return;

    update_history_gravity(search_state.capture_history[attacker][to][victim], delta);
}

static inline bool valid_history_piece(Piece pc)
{
    return pc != NO_PIECE && unsigned(pc) < PIECE_NB;
}

static inline int continuation_entry_score(const SearchStack& prev, Piece curPiece, Square curTo, int weight)
{
    if (!prev.current_move || !valid_history_piece(prev.moved_piece) || !valid_history_piece(curPiece))
        return 0;

    return search_state.cont_history[prev.moved_piece][to_sq(prev.current_move)][curPiece][curTo] * weight / 128;
}

static inline int continuation_history_score(const SearchStack* ss, int ply, Piece curPiece, Square curTo)
{
    int score = 0;

    if (ply >= 1)
        score += continuation_entry_score(ss[ply - 1], curPiece, curTo, MovePicker::CONTHIST1_WEIGHT);
    if (ply >= 2)
        score += continuation_entry_score(ss[ply - 2], curPiece, curTo, MovePicker::CONTHIST2_WEIGHT);
    if (ply >= 4)
        score += continuation_entry_score(ss[ply - 4], curPiece, curTo, MovePicker::CONTHIST4_WEIGHT);

    return score;
}

static inline void update_continuation_entry(const SearchStack& prev, Piece curPiece, Square curTo, int bonus, int base)
{
    if (!prev.current_move || !valid_history_piece(prev.moved_piece) || !valid_history_piece(curPiece))
        return;

    update_history_with_base(search_state.cont_history[prev.moved_piece][to_sq(prev.current_move)][curPiece][curTo], bonus, base);
}

static inline void update_continuation_histories(const SearchStack* ss, int ply, Move m, Piece movedPiece, int delta)
{
    const Square curTo = to_sq(m);
    const int base = continuation_history_score(ss, ply, movedPiece, curTo);

    if (ply >= 1)
        update_continuation_entry(ss[ply - 1], movedPiece, curTo, delta, base);

    if (ply >= 2)
        update_continuation_entry(ss[ply - 2], movedPiece, curTo, delta, base);

    if (ply >= 4)
        update_continuation_entry(ss[ply - 4], movedPiece, curTo, delta, base);
}

template <NodeType NT>
static int negamax(Position& pos, int depth, int alpha, int beta, int ply, SearchStack* ss, bool allow_nmp = true, bool cutNode = false);

static bool has_legal_move(Position& pos, Move excluded_move = 0);

static inline int score_to_tt(int score, int ply)
{
    if (score > MATE_SCORE - 1000) return score + ply;
    if (score < -MATE_SCORE + 1000) return score - ply;
    return score;
}

static inline int score_from_tt(int score, int ply, int halfmove_clock)
{
    const int remaining = std::max(0, 100 - halfmove_clock);

    if (score > MATE_SCORE - 1000) {
        if (MATE_SCORE - score > remaining)
            return MATE_SCORE - 1000;
        return score - ply;
    }

    if (score < -MATE_SCORE + 1000) {
        if (MATE_SCORE + score > remaining)
            return -MATE_SCORE + 1000;
        return score + ply;
    }

    return score;
}

static bool is_rule50_draw(Position& pos, bool in_check_now)
{
    if (pos.halfmove_clock() < 100)
        return false;

    return !in_check_now || has_legal_move(pos);
}

static bool is_immediate_draw(Position& pos, int ply, bool in_check_now)
{
    return is_rule50_draw(pos, in_check_now)
        || has_insufficient_material(pos)
        || (ply > 0 && pos.is_repetition_draw(ply));
}

static MovePicker::MainOrderData build_main_order_data(const SearchStack* ss, int ply)
{
    MovePicker::MainOrderData data{};
    data.history = search_state.history_heuristic;
    data.capture_history = search_state.capture_history;
    if (!ss || ply < 1)
        return data;

    const SearchStack& prev1 = ss[ply - 1];
    if (prev1.current_move && prev1.moved_piece != NO_PIECE)
        data.cont1 = search_state.cont_history[prev1.moved_piece][to_sq(prev1.current_move)];

    if (ply < 2)
        return data;

    const SearchStack& prev2 = ss[ply - 2];
    if (prev2.current_move && prev2.moved_piece != NO_PIECE)
        data.cont2 = search_state.cont_history[prev2.moved_piece][to_sq(prev2.current_move)];

    if (ply < 4)
        return data;

    const SearchStack& prev4 = ss[ply - 4];
    if (prev4.current_move && prev4.moved_piece != NO_PIECE)
        data.cont4 = search_state.cont_history[prev4.moved_piece][to_sq(prev4.current_move)];


    return data;
}

static inline void ensure_accumulator(const Position& pos, SearchStack* ss, int ply)
{
    if (!nnue::is_ready() || ss[ply].acc_valid)
        return;

    int valid_ply = ply - 1;
    while (valid_ply >= 0 && !ss[valid_ply].acc_valid)
        --valid_ply;

    if (valid_ply < 0) {
        nnue::refresh_acc(pos, WHITE, ss[ply].acc.white);
        nnue::refresh_acc(pos, BLACK, ss[ply].acc.black);
        ss[ply].acc_valid = true;
        return;
    }

    for (int i = valid_ply; i < ply; ++i) {
        ss[i + 1].acc = ss[i].acc;

        int hist_idx = pos.current_ply() - (ply - i);
        const nnue::DirtyPieces& dp = pos.state_at_ply(hist_idx).dp;

        nnue::apply_dirty(ss[i + 1].acc.white, WHITE, dp);
        nnue::apply_dirty(ss[i + 1].acc.black, BLACK, dp);
        ss[i + 1].acc_valid = true;
    }
}

static inline int eval_from_stack(const Position& pos, SearchStack* ss, int ply)
{
    int score = 0;

    if (nnue::is_ready()) {
        ensure_accumulator(pos, ss, ply);
        score = nnue::evaluate_from_pair(ss[ply].acc, pos);
    }
    else
        score = evaluate(pos);

    return clamp_eval_score(score);
}

static inline int scale_rule50_eval(int eval, const Position& pos)
{
    return eval * (200 - std::min(pos.halfmove_clock(), 200)) / 200;
}

static inline void seed_root_accumulator(const Position& pos, SearchStack* ss)
{
    ss[0].acc_valid = false;
    if (!nnue::is_ready())
        return;

    nnue::refresh_acc(pos, WHITE, ss[0].acc.white);
    nnue::refresh_acc(pos, BLACK, ss[0].acc.black);
    ss[0].acc_valid = true;
}

static void set_root_pv(RootMove& root_move, Move move, int child_ply)
{
    root_move.pv[0] = move;
    int child_len = search_state.pv_length[child_ply];
    if (child_len < 0 || child_len > MAX_PLY - 1)
        child_len = 0;

    for (int i = 0; i < child_len; ++i)
        root_move.pv[i + 1] = search_state.pv_table[child_ply][i];

    root_move.pv_length = child_len + 1;
}

static void copy_root_pv_to_main(const RootMove& root_move)
{
    const int len = std::clamp(root_move.pv_length, 0, MAX_PLY);
    for (int i = 0; i < len; ++i)
        search_state.pv_table[0][i] = root_move.pv[i];
    search_state.pv_length[0] = len;
}

static bool root_move_less(const RootMove& a, const RootMove& b)
{
    return a.score > b.score;
}

static int find_root_move_index(const RootSearchContext& root_context, Move move)
{
    for (int i = 0; i < root_context.root_count; ++i)
    {
        if (root_context.root_moves[i].move == move)
            return i;
    }

    return -1;
}

static int qsearch(Position& pos, int alpha, int beta, int ply, SearchStack* ss)
{
    if (ply > search_state.seldepth) search_state.seldepth = ply;
    search_state.nodes_count++;

    const uint64_t pollMaskQ = SEARCH_POLL_MASK;
    if ((search_state.nodes_count & pollMaskQ) == 0)
        update_hard_time();

    if (search_stop_requested())
        return alpha;

    bool inChk = in_check(pos, pos.side_to_move());
    if (is_immediate_draw(pos, ply, inChk))
        return draw_score();

    if (ply >= QS_MAX_PLY_GUARD)
        return inChk ? draw_score() : eval_from_stack(pos, ss, ply);


    search_state.pv_length[ply] = 0;
    int original_alpha = alpha;
    Move best_move = 0;

    uint64_t key = pos.hash();
    Move q_tt_move = 0;
    TTEntry* qtt = tt_probe(key);
    int q_tt_score = 0;
    bool has_q_tt_score = false;
    if (qtt) {
        q_tt_move = qtt->best_move;
        q_tt_score = score_from_tt(qtt->score, ply, pos.halfmove_clock());
        has_q_tt_score = true;

        if (pos.halfmove_clock() < 90) {
            if (qtt->flag == TT_EXACT)
                return q_tt_score;
            if (qtt->flag == TT_ALPHA && q_tt_score <= alpha)
                return q_tt_score;
            if (qtt->flag == TT_BETA && q_tt_score >= beta)
                return q_tt_score;
        }
    }

    int stand_pat = -INF;
    int raw_eval = -INF;
    if (!inChk)
    {
        raw_eval = qtt ? qtt->static_eval : eval_from_stack(pos, ss, ply);
        stand_pat = scale_rule50_eval(raw_eval, pos) + get_eval_correction(pos, ss, ply);

        stand_pat = clamp_eval_score(stand_pat);

        ss[ply].static_eval = stand_pat;

        if (has_q_tt_score) {
            if ((qtt->flag == TT_BETA && q_tt_score > stand_pat)
                || (qtt->flag == TT_ALPHA && q_tt_score < stand_pat)
                || qtt->flag == TT_EXACT)
                stand_pat = q_tt_score;
        }

        if (stand_pat >= beta) {
            tt_store(key, 0, score_to_tt(stand_pat, ply), TT_BETA, 0, raw_eval);
            return beta;
        }
        if (stand_pat > alpha) alpha = stand_pat;
    }

    MovePicker picker;
    picker.init_qsearch(pos, inChk, q_tt_move);

    int legal_moves = 0;

    for (Move m = picker.next(false); m; m = picker.next(false))
    {
        Piece movedPiece = pos.piece_on(from_sq(m));

        bool isQuiet = !is_capture(m) && !is_promotion(m);

        if (!inChk && isQuiet) {
            continue;
        }

        if (!inChk && is_capture(m) && !is_promotion(m)) {
            if (!movepick_see_ge(pos, m, -73)) {
                continue;
            }
        }

        if (!pos.make_move(m, true))
            continue;

        ss[ply + 1].acc_valid = false;

        tt_prefetch(pos.hash());
        ss[ply].current_move = m;
        ss[ply].moved_piece = movedPiece;
        legal_moves++;


        search_state.pv_length[ply + 1] = 0;

        int score = -qsearch(pos, -beta, -alpha, ply + 1, ss);
        pos.undo_move();

        if (search_stop_requested())
            return alpha;

        if (score >= beta) {
            int store_score = beta;
            tt_store(key, 0, score_to_tt(store_score, ply), TT_BETA, m, raw_eval);

            return beta;
        }
        if (score > alpha) {
            alpha = score;
            best_move = m;


            search_state.pv_table[ply][0] = m;
            int child_len = search_state.pv_length[ply + 1];
            if (child_len < 0 || child_len > MAX_PLY - ply - 1) child_len = 0;
            for (int j = 0; j < child_len; ++j)
                search_state.pv_table[ply][j + 1] = search_state.pv_table[ply + 1][j];
            search_state.pv_length[ply] = child_len + 1;
        }


    }

    if (inChk && legal_moves == 0)
        return -MATE_SCORE + ply;


    TTFlag flag;
    if (alpha <= original_alpha) flag = TT_ALPHA;
    else flag = TT_EXACT;

    tt_store(key, 0, score_to_tt(alpha, ply), flag, best_move, raw_eval);

    return alpha;
}


template <NodeType NT>
static int negamax(Position& pos, int depth, int alpha, int beta, int ply, SearchStack* ss, bool allow_nmp, bool cutNode)
{
    constexpr bool isPV = (NT == Root || NT == PV);
    constexpr bool isRoot = (NT == Root);

    if (ply > search_state.seldepth) search_state.seldepth = ply;

    if (search_stop_requested()) return alpha;
    bool inChk = in_check(pos, pos.side_to_move());
    if (ply >= MAX_PLY - 1) return inChk ? draw_score() : eval_from_stack(pos, ss, ply);
    search_state.nodes_count++;

    const uint64_t pollMaskN = SEARCH_POLL_MASK;
    if ((search_state.nodes_count & pollMaskN) == 0)
        update_hard_time();

    if (is_immediate_draw(pos, ply, inChk))
    {
        return draw_score();
    }

    alpha = std::max(alpha, -MATE_SCORE + ply);
    beta = std::min(beta, MATE_SCORE - ply - 1);
    if (alpha >= beta) return alpha;

    search_state.pv_length[ply] = 0;

    uint64_t key = pos.hash();
    int      original_alpha = alpha;
    TTEntry* entry = tt_probe(key);
    const bool tt_hit = entry != nullptr;
    const int tt_depth = tt_hit ? entry->depth : 0;
    const int tt_raw_score = tt_hit ? entry->score : 0;
    const TTFlag tt_flag = tt_hit ? entry->flag : TT_ALPHA;
    const Move tt_entry_move = tt_hit ? entry->best_move : Move(0);
    const int tt_static_eval = tt_hit ? entry->static_eval : 0;

    Move tt_move = tt_entry_move;
    ss[ply].tt_pv = tt_hit && tt_flag == TT_EXACT;

    int tt_score = tt_hit ? score_from_tt(tt_raw_score, ply, pos.halfmove_clock()) : 0;

    if constexpr (isRoot) {
        if (search_state.active_root_context
            && search_state.active_root_context->root_depth > 1
            && search_state.active_root_context->root_count > 0
            && search_state.active_root_context->root_moves[0].pv_length > 0)
            tt_move = search_state.active_root_context->root_moves[0].pv[0];
    }

    if (!isPV && tt_hit && tt_depth >= depth && ss[ply].excluded_move == 0)
    {
        if (pos.halfmove_clock() < 90) {
            if (tt_flag == TT_EXACT && ply > 0)
                return tt_score;
            if (tt_flag == TT_ALPHA && tt_score <= alpha) return tt_score;
            if (tt_flag == TT_BETA && tt_score >= beta)  return tt_score;
        }
    }

    if (inChk && depth <= 0)
        depth = 1;

    if (depth <= 0 && !inChk)
        return qsearch(pos, alpha, beta, ply, ss);

    // IIR
    if (!isRoot && depth >= 4 && tt_move == 0 && ss[ply].excluded_move == 0) {
        depth--;
    }

    int raw_eval = tt_hit ? tt_static_eval : eval_from_stack(pos, ss, ply);
    int staticEval = raw_eval;

    if (!inChk) {
        staticEval = scale_rule50_eval(staticEval, pos);
        staticEval += get_eval_correction(pos, ss, ply);

        if (pos.pieces(PAWN) == 0 && std::abs(staticEval) < MATE_SCORE - 1000) {
            int nonPawnMat = total_pieces(pos);
            if (nonPawnMat <= 3) {
                staticEval /= 2;
            }
            else if (nonPawnMat <= 5) {
                staticEval = (staticEval * 3) / 4;
            }
        }

        staticEval = clamp_eval_score(staticEval);
    }
    else
        staticEval = -INF;

    ss[ply].static_eval = staticEval;

    // Improving
    bool improving = false;
    if (!inChk) {
        if (ply >= 2 && ss[ply - 2].static_eval != -INF)
            improving = staticEval > ss[ply - 2].static_eval;
        else if (ply >= 4 && ss[ply - 4].static_eval != -INF)
            improving = staticEval > ss[ply - 4].static_eval;
    }

    if constexpr (!isPV) {
        // RFP
        if (!inChk && std::abs(beta) < MATE_SCORE - MAX_PLY && depth <= 3 && ss[ply].excluded_move == 0)
        {
            int rfp_margin = 120 * depth;
            if (staticEval - rfp_margin >= beta && staticEval < 10000)
                return staticEval;
        }

        // NMP
        bool prev_is_null = (ply > 0 && ss[ply - 1].current_move == 0);

        if (cutNode && allow_nmp && !inChk && !prev_is_null && ss[ply].excluded_move == 0 && depth >= 4 && ply >= search_state.nmp_min_ply && staticEval >= beta && staticEval < 10000 && has_non_pawn_material(pos, pos.side_to_move()))
        {
            int R = 3 + depth / 3 + std::min(2, (staticEval - beta) / 200);
            R = std::min(R, depth);

            ss[ply].current_move = 0;
            ss[ply].moved_piece = NO_PIECE;

            pos.do_null_move();
            ss[ply + 1].acc_valid = false;
            int null_score = -negamax<NonPV>(pos, depth - R - 1, -beta, -beta + 1, ply + 1, ss, true, false);
            pos.undo_null_move();

            if (search_stop_requested())
                return alpha;

            if (null_score >= beta)
            {
                if (depth >= 16 && search_state.nmp_min_ply == 0) {
                    const int previous_nmp_min_ply = search_state.nmp_min_ply;
                    search_state.nmp_min_ply = ply + 3 * (depth - R) / 4;

                    int verified_score = negamax<NonPV>(pos, depth - R, beta - 1, beta, ply, ss, false, false);
                    search_state.nmp_min_ply = previous_nmp_min_ply;

                    if (search_stop_requested())
                        return alpha;

                    if (verified_score >= beta) {
                        return null_score >= MATE_SCORE - MAX_PLY ? beta : null_score;
                    }
                }
                else {
                    return null_score >= MATE_SCORE - MAX_PLY ? beta : null_score;
                }
            }
        }

        // ProbCut
        if (!inChk && depth >= 5 && std::abs(beta) < MATE_SCORE - MAX_PLY && ss[ply].excluded_move == 0)
        {
            int probcut_beta = beta + 200 - (improving ? 50 : 0);
            int pc_count = 0;

            MovePicker pc_picker;
            pc_picker.init_qsearch(pos, inChk, tt_move);

            for (Move m = pc_picker.next(false); m; m = pc_picker.next(false))
            {
                if (pc_count++ >= 3) break;

                if (movepick_see_ge(pos, m, 0))
                {
                    Piece movedPiece = pos.piece_on(from_sq(m));
                    if (pos.make_move(m, true))
                    {
                        ss[ply + 1].acc_valid = false;
                        ss[ply].current_move = m;
                        ss[ply].moved_piece = movedPiece;

                        int score = -qsearch(pos, -probcut_beta, -probcut_beta + 1, ply + 1, ss);

                        if (score >= probcut_beta)
                        {
                            score = -negamax<NonPV>(pos, depth - 4, -probcut_beta, -probcut_beta + 1, ply + 1, ss);
                        }

                        pos.undo_move();

                        if (search_stop_requested())
                        {
                            ss[ply].current_move = 0;
                            ss[ply].moved_piece = NO_PIECE;
                            return alpha;
                        }

                        if (score >= probcut_beta)
                        {
                            int store_score = score >= MATE_SCORE - MAX_PLY ? beta : score;
                            tt_store(key, depth - 3, score_to_tt(store_score, ply), TT_BETA, m, raw_eval);

                            ss[ply].current_move = 0;
                            ss[ply].moved_piece = NO_PIECE;
                            return store_score;
                        }
                    }
                }
            }
            ss[ply].current_move = 0;
            ss[ply].moved_piece = NO_PIECE;
        }
    }

    Move counter_move = 0;
    if (ply >= 1)
    {
        const Move prev = ss[ply - 1].current_move;
        if (prev)
            counter_move = search_state.countermove[from_sq(prev)][to_sq(prev)];
    }


    const MovePicker::MainOrderData orderData = build_main_order_data(ss, ply);
    MovePicker picker;
    picker.init_main(pos,
        tt_move,
        search_state.killer_moves[0][ply],
        search_state.killer_moves[1][ply],
        counter_move,
        &orderData);

    int  legal_moves = 0;
    int  moveCount = 0;
    Move best_move = 0;
    int  best_score = -INF;

    SearchMoveBufferScope move_buffer_scope;
    SearchMoveBuffer& move_buffer = *move_buffer_scope.buffer;

    Move* quiet_searched = move_buffer.quiet_searched;
    Piece* quiet_pieces = move_buffer.quiet_pieces;
    int  quiet_count = 0;

    Move* capture_searched = move_buffer.capture_searched;
    Piece* capture_attackers = move_buffer.capture_attackers;
    Piece* capture_victims = move_buffer.capture_victims;
    int  capture_count = 0;

    bool skip_quiets = false;
    bool pruned_or_skipped_move = false;

    while (true)
    {
        if constexpr (isRoot) {
            if (!search_state.active_root_context)
                break;
        }

        Move m = picker.next(skip_quiets);
        if (!m)
            break;

        int root_state_index = -1;

        if constexpr (isRoot) {
            root_state_index = find_root_move_index(*search_state.active_root_context, m);
            if (root_state_index < 0)
                continue;
        }

        if (m == ss[ply].excluded_move) continue;

        bool isQuiet = !is_capture(m) && !is_promotion(m);

        if (skip_quiets && isQuiet) {
            pruned_or_skipped_move = true;
            continue;
        }

        Piece movedPiece = pos.piece_on(from_sq(m));
        Piece capturedPiece = captured_piece_for_move(pos, m);

        int hist_score = 0;
        if (isQuiet) {
            const Square to = to_sq(m);
            hist_score = search_state.history_heuristic[from_sq(m)][to]
                + continuation_history_score(ss, ply, movedPiece, to);
        }
        else {
            hist_score = capture_history_score(pos, m);
        }

        // LMP
        if (!isRoot && !inChk && isQuiet && depth <= 8 && std::abs(alpha) < MATE_SCORE - MAX_PLY && moveCount > 0) {
            int lmp_threshold = (3 + depth * depth) / (improving ? 1 : 2);
            if (moveCount >= lmp_threshold) {
                skip_quiets = true;
                pruned_or_skipped_move = true;
                continue;
            }
        }

        if constexpr (!isPV) {
            if (!inChk && isQuiet && depth <= 8 && std::abs(alpha) < MATE_SCORE - MAX_PLY) {
                int fp_margin = 120 * depth;
                if (m != search_state.killer_moves[0][ply] && m != search_state.killer_moves[1][ply] && hist_score < 4000) {
                    const int futility_score = staticEval + fp_margin;
                    if (futility_score <= alpha) {
                        best_score = std::max(best_score, futility_score);
                        pruned_or_skipped_move = true;
                        continue;
                    }
                }
            }
        }

        if (!isRoot && depth <= 8 && !inChk) {
            int see_threshold = isQuiet ? -50 * depth : -100 * depth;

            if (!movepick_see_ge(pos, m, see_threshold)) {
                pruned_or_skipped_move = true;
                continue;
            }
        }

        const bool givesChk = pos.gives_check(m);

        int ext = 0;
        const bool tt_has_lower_bound = tt_flag == TT_BETA || tt_flag == TT_EXACT;
        if (!isRoot
            && m == tt_move
            && depth >= 6 + (ss[ply].tt_pv ? 1 : 0)
            && ss[ply].excluded_move == 0
            && tt_hit
            && tt_depth >= depth - 3
            && tt_has_lower_bound
            && std::abs(tt_score) < MATE_SCORE - MAX_PLY
            && !is_shuffling(m, ss, ply, pos))
        {
            if (!pos.make_move(m, true))
                continue;
            pos.undo_move();

            int singular_beta = tt_score - (depth * SINGULAR_BETA_MARGIN) / 64;

            ss[ply].excluded_move = m;
            int singular_score = negamax<NonPV>(pos, (depth - 1) / 2, singular_beta - 1, singular_beta, ply, ss, false, cutNode);
            ss[ply].excluded_move = 0;

            if (search_stop_requested())
                return alpha;

            if (singular_score < singular_beta) {
                if (!isPV && singular_score < singular_beta - SINGULAR_DOUBLE_EXT_MARGIN)
                    ext = 2 + (isQuiet && singular_score < singular_beta - SINGULAR_TRIPLE_EXT_MARGIN);
                else
                    ext = 1;
            }
            else if (singular_score >= beta
                && std::abs(singular_score) < MATE_SCORE - MAX_PLY) {
                return singular_score;
            }
            else if (tt_score >= beta) {
                ext = isPV ? -2 : -3;
            }
            else if (cutNode) {
                ext = -2;
            }
        }

        if (!pos.make_move(m, true))
            continue;

        ss[ply + 1].acc_valid = false;

        tt_prefetch(pos.hash());

        ss[ply].current_move = m;
        ss[ply].moved_piece = movedPiece;

        legal_moves++;
        moveCount++;

        if (isQuiet && quiet_count < 256)
        {
            quiet_searched[quiet_count] = m;
            quiet_pieces[quiet_count] = movedPiece;
            quiet_count++;
        }
        else if (!isQuiet && capture_count < 256)
        {
            capture_searched[capture_count] = m;
            capture_attackers[capture_count] = movedPiece;
            capture_victims[capture_count] = capturedPiece;
            capture_count++;
        }

        int  score;

        int searchedDepth = std::min(MAX_PLY - 1, depth - 1 + ext);
        const uint64_t root_move_nodes_before = isRoot ? search_state.nodes_count : 0;

        if (moveCount == 1) {
            search_state.pv_length[ply + 1] = 0;
            score = -negamax<isPV ? PV : NonPV>(
                pos, searchedDepth, -beta, -alpha, ply + 1, ss, true, isPV ? false : !cutNode);
        }
        else {
            search_state.pv_length[ply + 1] = 0;

            int R = 0;

            // LMR
            if (depth >= 3 && moveCount > (isPV ? 3 : 1) && !inChk && ext == 0)
            {
                int d = std::min(depth, 63);
                int c = std::min(moveCount, 255);

                int R_scaled = LMR_TABLE[isQuiet ? 1 : 0][d][c];

                if constexpr (isPV) R_scaled -= 1 * LMR_SCALE;

                if (ss[ply].tt_pv) {
                    R_scaled -= 1 * LMR_SCALE;
                }

                if (tt_hit && tt_depth >= depth) {
                    R_scaled -= 1 * LMR_SCALE;
                }

                if (tt_move != 0 && (is_capture(tt_move) || is_promotion(tt_move))) {
                    R_scaled += 1 * LMR_SCALE;
                }


                if (isQuiet) {

                    int hist_modifier = (hist_score * LMR_SCALE) / 8192;
                    R_scaled -= std::clamp(hist_modifier, -3 * LMR_SCALE, 3 * LMR_SCALE);


                    if (m == search_state.killer_moves[0][ply] || m == search_state.killer_moves[1][ply]) {
                        R_scaled -= 1 * LMR_SCALE;
                    }

                    if (ply >= 1 && ss[ply - 1].current_move != 0) {
                        Move prev = ss[ply - 1].current_move;
                        if (m == search_state.countermove[from_sq(prev)][to_sq(prev)]) {
                            R_scaled -= 1 * LMR_SCALE;
                        }
                    }
                }
                else {

                    int hist_modifier = (hist_score * LMR_SCALE) / 16384;
                    R_scaled -= std::clamp(hist_modifier, -2 * LMR_SCALE, 2 * LMR_SCALE);
                }

                if (!improving) {
                    R_scaled += 1 * LMR_SCALE;
                }

                if (cutNode) {
                    R_scaled += (ss[ply].tt_pv ? 1 : 2) * LMR_SCALE;
                }

                if (givesChk) {
                    R_scaled -= LMR_CHECK_REDUCTION;
                }

                R = R_scaled / LMR_SCALE;
                R = std::clamp(R, 0, searchedDepth - 1);
            }

            if (R > 0) {
                score = -negamax<NonPV>(pos, searchedDepth - R, -(alpha + 1), -alpha, ply + 1, ss, true, true);


                if (score > alpha) {
                    int new_depth = searchedDepth;

                    if (score > alpha + 50) {
                        new_depth += 1;
                    }

                    score = -negamax<NonPV>(pos, new_depth, -(alpha + 1), -alpha, ply + 1, ss, true, !cutNode);
                }
            }
            else {
                score = -negamax<NonPV>(pos, searchedDepth, -(alpha + 1), -alpha, ply + 1, ss, true, !cutNode);
            }

            if constexpr (isPV) {
                if (score > alpha && score < beta) {
                    search_state.pv_length[ply + 1] = 0;
                    score = -negamax<PV>(pos, searchedDepth, -beta, -alpha, ply + 1, ss, true, false);
                }
            }
        }

        pos.undo_move();

        if constexpr (isRoot) {
            if (search_state.active_root_context && root_state_index >= 0) {
                const uint64_t move_nodes = search_state.nodes_count - root_move_nodes_before;
                RootMove& root_move = search_state.active_root_context->root_moves[root_state_index];
                root_move.window_score = score;
                root_move.nodes += move_nodes;

                if (moveCount == 1 || score > alpha) {
                    root_move.score = score;
                    root_move.uci_score = score;
                    root_move.lowerbound = false;
                    root_move.upperbound = false;

                    if (score <= alpha) {
                        root_move.uci_score = alpha;
                        root_move.upperbound = true;
                    }
                    else if (score >= beta) {
                        root_move.uci_score = beta;
                        root_move.lowerbound = true;
                    }

                    root_move.seldepth = search_state.seldepth;
                    set_root_pv(root_move, m, ply + 1);
                }
                else {
                    root_move.score = -INF;
                }

                if (!search_stop_requested() && score > search_state.active_root_context->best_score) {
                    search_state.active_root_context->best_score = score;
                    search_state.active_root_context->best_move = m;
                    search_state.active_root_context->best_move_nodes = root_move.nodes;
                }
            }
        }

        if (search_stop_requested()) break;

        best_score = std::max(best_score, score);

        if constexpr (!isRoot) {
            if (score >= beta)
            {
                const int histDepth = depth
                    + (!inChk && staticEval <= original_alpha ? 1 : 0)
                    + (score > beta + 209 ? 1 : 0);
                const int bonus = stat_bonus(histDepth);
                const int malus = stat_malus(histDepth);

                if (isQuiet)
                {
                    search_state.killer_moves[1][ply] = search_state.killer_moves[0][ply];
                    search_state.killer_moves[0][ply] = m;

                    update_history_gravity(search_state.history_heuristic[from_sq(m)][to_sq(m)], bonus);

                    if (ply >= 1)
                    {
                        Move prev = ss[ply - 1].current_move;
                        if (prev)
                            search_state.countermove[from_sq(prev)][to_sq(prev)] = m;
                    }

                    update_continuation_histories(ss, ply, m, movedPiece, bonus);

                    for (int i = 0; i < quiet_count; ++i)
                    {
                        Move qm = quiet_searched[i];
                        Piece qp = quiet_pieces[i];
                        if (!qm || qm == m || qp == NO_PIECE) continue;

                        update_history_gravity(search_state.history_heuristic[from_sq(qm)][to_sq(qm)], -malus);
                        update_continuation_histories(ss, ply, qm, qp, -malus);
                    }

                    for (int i = 0; i < capture_count; ++i)
                    {
                        Move cm = capture_searched[i];
                        Piece attacker = capture_attackers[i];
                        Piece victim = capture_victims[i];
                        if (!cm || attacker == NO_PIECE) continue;

                        update_capture_history(attacker, to_sq(cm), victim, -malus);
                    }
                }
                else
                {
                    update_capture_history(movedPiece, to_sq(m), capturedPiece, bonus);

                    for (int i = 0; i < capture_count; ++i)
                    {
                        Move cm = capture_searched[i];
                        Piece attacker = capture_attackers[i];
                        Piece victim = capture_victims[i];
                        if (!cm || cm == m || attacker == NO_PIECE) continue;

                        update_capture_history(attacker, to_sq(cm), victim, -malus);
                    }
                }

                int store_score = score;
                if (depth > 0
                    && std::abs(score) < MATE_SCORE - 1000
                    && std::abs(beta) < MATE_SCORE - 1000) {
                    store_score = (score * depth + beta) / (depth + 1);
                }

                if (ss[ply].excluded_move == 0) {
                    tt_store(key, depth, score_to_tt(store_score, ply), TT_BETA, m, raw_eval);
                }

                if (!inChk && !is_capture(m)
                    && std::abs(score) < MATE_SCORE - 1000
                    && score > staticEval) {
                    if (ss[ply].excluded_move == 0) {
                        update_eval_correction(pos, ss, ply, depth, score - staticEval);
                    }
                }

                return score;
            }
        }

        if (score > alpha)
        {
            alpha = score;
            best_move = m;

            search_state.pv_table[ply][0] = m;
            int child_len = search_state.pv_length[ply + 1];
            if (child_len < 0 || child_len > MAX_PLY - ply - 1) child_len = 0;
            for (int j = 0; j < child_len; ++j)
                search_state.pv_table[ply][j + 1] = search_state.pv_table[ply + 1][j];
            search_state.pv_length[ply] = child_len + 1;
        }

        if constexpr (isRoot) {
            if (score >= beta)
                break;
        }
    }
    if (search_stop_requested())
        return alpha;

    if (legal_moves == 0) {
        if (ss[ply].excluded_move != 0)
            return alpha;

        if (pruned_or_skipped_move && has_legal_move(pos))
            return best_score != -INF ? best_score : alpha;

        return inChk ? -MATE_SCORE + ply : 0;
    }

    const bool root_has_best = isRoot && search_state.active_root_context && search_state.active_root_context->best_move != 0;
    const int node_score = root_has_best ? search_state.active_root_context->best_score
        : (best_score != -INF ? best_score : alpha);
    if (root_has_best && best_move == 0)
        best_move = search_state.active_root_context->best_move;

    if (!search_stop_requested() && best_move != 0)
    {
        const int histDepth = depth
            + (!inChk && staticEval <= original_alpha ? 1 : 0)
            + (node_score > beta + 209 ? 1 : 0);
        const int bonus = stat_bonus(histDepth);
        const int malus = stat_malus(histDepth);

        const bool best_is_quiet = best_move && !is_capture(best_move) && !is_promotion(best_move);
        const bool best_is_capture = best_move && (is_capture(best_move) || is_promotion(best_move));

        for (int i = 0; i < quiet_count; ++i)
        {
            Move qm = quiet_searched[i];
            Piece qp = quiet_pieces[i];
            if (!qm || qp == NO_PIECE) continue;

            const bool isBest = (best_is_quiet && qm == best_move);
            const int delta = isBest ? bonus : -malus;

            update_history_gravity(search_state.history_heuristic[from_sq(qm)][to_sq(qm)], delta);

            update_continuation_histories(ss, ply, qm, qp, delta);

            if (isBest && ply >= 1)
            {
                Move prev = ss[ply - 1].current_move;
                if (prev)
                    search_state.countermove[from_sq(prev)][to_sq(prev)] = qm;
            }
        }

        for (int i = 0; i < capture_count; ++i)
        {
            Move cm = capture_searched[i];
            Piece attacker = capture_attackers[i];
            Piece victim = capture_victims[i];
            if (!cm || attacker == NO_PIECE) continue;

            const bool isBest = (best_is_capture && cm == best_move);
            const int delta = isBest ? bonus : -malus;
            update_capture_history(attacker, to_sq(cm), victim, delta);
        }
    }

    TTFlag flag;
    if (node_score <= original_alpha) flag = TT_ALPHA;
    else if (node_score >= beta)      flag = TT_BETA;
    else                         flag = TT_EXACT;

    if (ss[ply].excluded_move == 0) {
        tt_store(key, depth, score_to_tt(node_score, ply), flag, best_move, raw_eval);
    }

    if (!inChk && !(best_move && is_capture(best_move)) && std::abs(node_score) < MATE_SCORE - 1000) {
        bool skip_update = false;
        if (ss[ply].excluded_move != 0) skip_update = true;
        if (flag == TT_ALPHA && node_score >= staticEval) skip_update = true;
        if (flag == TT_BETA && node_score <= staticEval) skip_update = true;

        if (!skip_update) {
            update_eval_correction(pos, ss, ply, depth, node_score - staticEval);
        }
    }

    return node_score;
}

static void prepare_root_moves_for_depth(RootMove* root_moves, int root_count)
{
    for (int i = 0; i < root_count; ++i)
        root_moves[i].previous_score = root_moves[i].score;
}

static void initialize_search_stack(SearchStack* ss)
{
    for (int i = 0; i < MAX_PLY + 4; ++i)
    {
        ss[i].current_move = 0;
        ss[i].moved_piece = NO_PIECE;
        ss[i].static_eval = -INF;
        ss[i].tt_pv = false;
        ss[i].excluded_move = 0;
        ss[i].acc_valid = false;
    }
}

static void generate_legal_root_moves(Position& pos, MoveList& root_moves)
{
    MoveList pseudo_legal_root_moves;
    generate_moves(pos, pseudo_legal_root_moves);

    root_moves.size = 0;
    for (int i = 0; i < pseudo_legal_root_moves.size; ++i)
    {
        Move m = pseudo_legal_root_moves.moves[i];
        if (pos.make_move(m, true))
        {
            root_moves.push(m);
            pos.undo_move();
        }
    }
}

static bool has_legal_move(Position& pos, Move excluded_move)
{
    MoveList pseudo_legal_moves;
    generate_moves(pos, pseudo_legal_moves);

    for (int i = 0; i < pseudo_legal_moves.size; ++i)
    {
        Move m = pseudo_legal_moves.moves[i];
        if (!m || m == excluded_move)
            continue;

        if (pos.make_move(m, true))
        {
            pos.undo_move();
            return true;
        }
    }

    return false;
}

static void print_search_info(Position& pos, int depth, int best_score)
{
    if (g_silent_search)
        return;

    int time_spent = tm.elapsed();
    uint64_t nps = (time_spent > 0)
        ? (search_state.nodes_count * 1000ULL / time_spent)
        : 0;

    std::cout << "info depth " << depth << " seldepth " << search_state.seldepth;

    if (best_score > MATE_SCORE - 1000)
    {
        int moves_to_mate = (MATE_SCORE - best_score + 1) / 2;
        std::cout << " score mate " << moves_to_mate;

        if (g_uci_show_wdl.load(std::memory_order_relaxed))
            std::cout << " wdl 1000 0 0";
    }
    else if (best_score < -MATE_SCORE + 1000)
    {
        int moves_to_mate = -(MATE_SCORE + best_score) / 2;
        std::cout << " score mate " << moves_to_mate;

        if (g_uci_show_wdl.load(std::memory_order_relaxed))
            std::cout << " wdl 0 0 1000";
    }
    else
    {
        std::cout << " score cp " << wdl::normalize_score(best_score, pos);

        if (g_uci_show_wdl.load(std::memory_order_relaxed))
        {
            const wdl::WDL model = wdl::model(best_score, pos);
            std::cout << " wdl " << model.win << " " << model.draw << " " << model.loss;
        }
    }

    std::cout << " nodes " << search_state.nodes_count
        << " nps " << nps
        << " hashfull " << tt_hashfull()
        << " time " << time_spent
        << " pv ";

    int pv_made = 0;
    for (int i = 0; i < search_state.pv_length[0]; ++i)
    {
        Move mv = search_state.pv_table[0][i];
        if (!pos.make_move(mv))
            break;
        ++pv_made;

        Square f = from_sq(mv);
        Square t = to_sq(mv);
        std::cout << char('a' + file_of(f)) << char('1' + rank_of(f))
            << char('a' + file_of(t)) << char('1' + rank_of(t));
        if (is_promotion(mv))
        {
            PieceType pt = promotion_type(mv);
            if (pt == QUEEN)  std::cout << "q";
            else if (pt == ROOK)   std::cout << "r";
            else if (pt == BISHOP) std::cout << "b";
            else if (pt == KNIGHT) std::cout << "n";
        }
        std::cout << " ";
    }
    while (pv_made-- > 0)
        pos.undo_move();
    std::cout << std::endl;
}

SearchResult search(Position& pos,
    int max_depth,
    int movetime,
    int time_left,
    int increment,
    int moves_to_go,
    bool infinite,
    uint64_t max_nodes,
    int move_overhead,
    bool soft_node_limit,
    uint64_t stop_epoch_baseline)
{
    search_state.local_stop_epoch = stop_epoch_baseline == UINT64_MAX
        ? global_stop_epoch.load(std::memory_order_relaxed)
        : stop_epoch_baseline;
    search_state.stop_flag = false;
    search_state.nodes_count = 0;
    search_state.target_max_nodes = max_nodes;
    search_state.target_nodes_soft = soft_node_limit;
    search_state.seldepth = 0;
    search_state.nmp_min_ply = 0;

    std::unique_ptr<SearchStack[]> ss_storage = std::make_unique<SearchStack[]>(MAX_PLY + 4);
    SearchStack* ss = ss_storage.get();
    initialize_search_stack(ss);
    seed_root_accumulator(pos, ss);

    for (int i = 0; i < MAX_PLY; ++i) search_state.pv_length[i] = 0;
    for (int i = 0; i < MAX_PLY; ++i)
    {
        search_state.killer_moves[0][i] = 0;
        search_state.killer_moves[1][i] = 0;
    }

    tm.reset();
    tt_new_search();

    int current_game_ply = std::max(0, (pos.fullmove_number() - 1) * 2
        + (pos.side_to_move() == BLACK ? 1 : 0));

    tm.set_limits(
        infinite,
        movetime,
        time_left,
        increment,
        moves_to_go,
        current_game_ply,
        move_overhead
    );

    tm.start();

    SearchResult result{};
    result.best_move = 0;
    result.score = 0;
    result.depth = 0;
    result.nodes = 0;

    MoveList legal_root_moves;
    generate_legal_root_moves(pos, legal_root_moves);
    std::unique_ptr<RootMove[]> root_moves_storage = std::make_unique<RootMove[]>(MAX_MOVES);
    RootMove* root_moves = root_moves_storage.get();
    const int root_count = legal_root_moves.size;
    for (int i = 0; i < root_count; ++i)
    {
        root_moves[i].move = legal_root_moves.moves[i];
        root_moves[i].pv[0] = root_moves[i].move;
        root_moves[i].pv_length = 1;
    }

    if (root_count == 0)
    {
        result.best_move = 0;
        result.score = in_check(pos, pos.side_to_move()) ? -MATE_SCORE : 0;
        result.depth = 0;
        result.nodes = search_state.nodes_count;
        return result;
    }


    const bool root_in_check = in_check(pos, pos.side_to_move());
    if (is_rule50_draw(pos, root_in_check) || has_insufficient_material(pos) || pos.is_repetition_draw(0))
    {
        result.best_move = root_moves[0].move;
        result.score = draw_score();
        result.depth = 0;
        result.nodes = search_state.nodes_count;
        return result;
    }

    result.best_move = root_moves[0].move;

    for (int depth = 1; depth <= max_depth; ++depth)
    {
        search_state.seldepth = 0;


        update_hard_time();
        if (search_stop_requested())
            break;

        prepare_root_moves_for_depth(root_moves, root_count);

        int delta = ROOT_ASPIRATION_DELTA_BASE;

        int alpha = -INF;
        int beta = INF;

        if (depth >= ROOT_ASPIRATION_DEPTH && root_moves[0].window_score != -INF) {
            alpha = std::max(-INF, root_moves[0].window_score - delta);
            beta = std::min(INF, root_moves[0].window_score + delta);
        }

        int best_score = -INF;
        Move current_depth_best_move = 0;
        uint64_t current_depth_best_move_nodes = 0;
        int aspiration_reduction = 0;

        while (true) {
            search_state.pv_length[0] = 0;

            RootSearchContext root_context{};
            root_context.root_moves = root_moves;
            root_context.root_count = root_count;
            root_context.root_depth = depth;

            search_state.active_root_context = &root_context;
            const int adjusted_depth = std::max(1, depth - aspiration_reduction);
            const int root_score = negamax<Root>(pos, adjusted_depth, alpha, beta, 0, ss);
            search_state.active_root_context = nullptr;

            if (search_stop_requested()) break;

            best_score = (root_context.best_move != 0) ? root_context.best_score : root_score;
            std::stable_sort(root_moves, root_moves + root_count, root_move_less);
            if (root_moves[0].pv_length > 0)
                copy_root_pv_to_main(root_moves[0]);

            if (best_score <= alpha) {
                beta = (alpha + beta) / 2;
                alpha = std::max(-INF, best_score - delta);
                aspiration_reduction = 0;
            }
            else if (best_score >= beta) {
                beta = std::min(INF, best_score + delta);
                aspiration_reduction = std::min(aspiration_reduction + 1, ROOT_ASPIRATION_REDUCTION_MAX);
            }
            else {
                current_depth_best_move = root_moves[0].move;
                current_depth_best_move_nodes = root_moves[0].nodes;
                break;
            }

            delta += delta * ROOT_ASPIRATION_WIDENING_FACTOR / 16;

        }

        if (search_stop_requested()) break;

        if (current_depth_best_move != 0)
        {
            result.best_move = current_depth_best_move;
            result.score = best_score;
            result.depth = depth;
            result.nodes = search_state.nodes_count;

            if (!infinite && movetime <= 0 && time_left > 0)
                tm.update_after_iteration(depth,
                    current_depth_best_move,
                    best_score,
                    current_depth_best_move_nodes,
                    search_state.nodes_count,
                    root_count);
        }

        print_search_info(pos, depth, best_score);

        if (tm.should_stop()) {
            break;
        }

        if (best_score >= MATE_SCORE - 4 || best_score <= -MATE_SCORE + 4)
            break;

        if (search_state.target_max_nodes > 0 && search_state.nodes_count >= search_state.target_max_nodes) {
            break;
        }

    }

    return result;
}
