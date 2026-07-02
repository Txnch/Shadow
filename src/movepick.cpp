#include "movepick.h"
#include "evaluate.h"
#include "movegen.h"
#include "bitboard.h"
#include "position.h"

#include <algorithm>

namespace {

    static constexpr int PIECE_VALUES[PIECE_TYPE_NB] = {
        0, 100, 300, 300, 500, 900, 0
    };

    static inline int value_of(PieceType pt) {
        return unsigned(pt) < PIECE_TYPE_NB ? PIECE_VALUES[pt] : 0;
    }

    static inline Bitboard lsb_bb(Bitboard b) {
        return b & (0ULL - b);
    }

    inline Square pop_lsb_local(Bitboard& b) {
        Square s = lsb(b);
        b &= b - 1;
        return s;
    }

    static inline Bitboard all_attackers_to(
        Square sq,
        Bitboard occ,
        const Bitboard colorPieces[COLOR_NB],
        Bitboard pawns,
        Bitboard knights,
        Bitboard bishopsQueens,
        Bitboard rooksQueens,
        Bitboard kings)
    {
        return (PawnAttacks[BLACK][sq] & colorPieces[WHITE] & pawns)
            | (PawnAttacks[WHITE][sq] & colorPieces[BLACK] & pawns)
            | (KnightAttacks[sq] & knights)
            | (KingAttacks[sq] & kings)
            | (get_bishop_attacks(sq, occ) & bishopsQueens)
            | (get_rook_attacks(sq, occ) & rooksQueens);
    }

    static bool see_ge(const Position& pos, Move m, int threshold) {
        if (is_en_passant(m) || is_castling(m) || is_promotion(m))
            return threshold <= 0;

        Square from = from_sq(m);
        Square to = to_sq(m);

        const Piece mover = pos.piece_on(from);
        if (mover == NO_PIECE)
            return false;

        const Bitboard colorPieces[COLOR_NB] = {
            pos.pieces(WHITE),
            pos.pieces(BLACK)
        };

        const Bitboard pawns = pos.pieces(PAWN);
        const Bitboard knights = pos.pieces(KNIGHT);
        const Bitboard bishops = pos.pieces(BISHOP);
        const Bitboard rooks = pos.pieces(ROOK);
        const Bitboard queens = pos.pieces(QUEEN);
        const Bitboard kings = pos.pieces(KING);
        const Bitboard bishopsQueens = bishops | queens;
        const Bitboard rooksQueens = rooks | queens;

        int swap = (is_capture(m) ? value_of(piece_type(pos.piece_on(to))) : 0) - threshold;
        if (swap < 0)
            return false;

        swap = value_of(piece_type(mover)) - swap;
        if (swap <= 0)
            return true;

        Bitboard occupied = pos.all_pieces() ^ square_bb(from) ^ square_bb(to);
        Bitboard attackers = all_attackers_to(to, occupied, colorPieces, pawns, knights, bishopsQueens, rooksQueens, kings);
        Color stm = pos.side_to_move();
        int res = 1;

        while (true) {
            stm = ~stm;
            attackers &= occupied;

            Bitboard stmAttackers = attackers & colorPieces[stm];
            if (!stmAttackers)
                break;

            if (pos.pinners(~stm) & occupied) {
                stmAttackers &= ~pos.blockers_for_king(stm);
                if (!stmAttackers)
                    break;
            }

            res ^= 1;

            Bitboard bb = stmAttackers & pawns;
            if (bb) {
                if ((swap = PIECE_VALUES[PAWN] - swap) < res)
                    break;
                occupied ^= lsb_bb(bb);
                attackers |= get_bishop_attacks(to, occupied) & bishopsQueens;
            }
            else if ((bb = stmAttackers & knights) != 0) {
                if ((swap = PIECE_VALUES[KNIGHT] - swap) < res)
                    break;
                occupied ^= lsb_bb(bb);
            }
            else if ((bb = stmAttackers & bishops) != 0) {
                if ((swap = PIECE_VALUES[BISHOP] - swap) < res)
                    break;
                occupied ^= lsb_bb(bb);
                attackers |= get_bishop_attacks(to, occupied) & bishopsQueens;
            }
            else if ((bb = stmAttackers & rooks) != 0) {
                if ((swap = PIECE_VALUES[ROOK] - swap) < res)
                    break;
                occupied ^= lsb_bb(bb);
                attackers |= get_rook_attacks(to, occupied) & rooksQueens;
            }
            else if ((bb = stmAttackers & queens) != 0) {
                swap = PIECE_VALUES[QUEEN] - swap;
                occupied ^= lsb_bb(bb);
                attackers |= (get_bishop_attacks(to, occupied) & bishopsQueens)
                    | (get_rook_attacks(to, occupied) & rooksQueens);
            }
            else {
                return (attackers & ~colorPieces[stm]) ? bool(res ^ 1) : bool(res);
            }
        }

        return bool(res);
    }

    static int capture_mvv_lva_internal(const Position& pos, Move m) {
        if (!is_capture(m))
            return 0;

        const Piece attacker = pos.piece_on(from_sq(m));

        Piece victim = pos.piece_on(to_sq(m));
        if (is_en_passant(m))
            victim = make_piece(~pos.side_to_move(), PAWN);

        return value_of(piece_type(victim)) * 16 - value_of(piece_type(attacker));
    }

} // namespace

int movepick_capture_mvv_lva(const Position& pos, Move m) {
    return capture_mvv_lva_internal(pos, m);
}

bool movepick_see_ge(const Position& pos, Move m, int threshold) {
    return see_ge(pos, m, threshold);
}

MovePicker::MovePicker() = default;

int MovePicker::score_main_move(const Position& pos, Move m, const MovePicker::MainOrderData& order_data) {
    if (is_capture(m) || is_promotion(m)) {
        int score = 200000;
        if (is_capture(m)) score += movepick_capture_mvv_lva(pos, m);

        if (order_data.capture_history) {
            const Piece attacker = pos.piece_on(from_sq(m));
            Piece victim = NO_PIECE;

            if (is_capture(m)) {
                victim = pos.piece_on(to_sq(m));
                if (is_en_passant(m))
                    victim = make_piece(~pos.side_to_move(), PAWN);
            }

            if (attacker != NO_PIECE)
                score += order_data.capture_history[attacker][to_sq(m)][victim] / 16;
        }
        return score;
    }

    int score = 0;
    if (order_data.history)
        score += order_data.history[from_sq(m)][to_sq(m)];

    const Piece curPiece = pos.piece_on(from_sq(m));
    if (curPiece != NO_PIECE) {
        if (order_data.cont1)
            score += order_data.cont1[curPiece][to_sq(m)] * CONTHIST1_WEIGHT / 128;
        if (order_data.cont2)
            score += order_data.cont2[curPiece][to_sq(m)] * CONTHIST2_WEIGHT / 128;
        if (order_data.cont4)
            score += order_data.cont4[curPiece][to_sq(m)] * CONTHIST4_WEIGHT / 128;
    }
    const PieceType pt = piece_type(pos.piece_on(from_sq(m)));
    if ((pos.check_squares(pt) & square_bb(to_sq(m))) && movepick_see_ge(pos, m, -75))
        score += 16384;

    return score;
}

void MovePicker::select_best(int begin, int end) {
    int bestIdx = begin;
    int bestScore = scores[begin];
    for (int i = begin + 1; i < end; ++i) {
        if (scores[i] > bestScore) {
            bestScore = scores[i];
            bestIdx = i;
        }
    }
    if (bestIdx != begin) {
        std::swap(moves[begin], moves[bestIdx]);
        std::swap(scores[begin], scores[bestIdx]);
    }
}

void MovePicker::init_main(const Position& pos,
    Move tt_move,
    Move killer1,
    Move killer2,
    Move counter_move,
    const MainOrderData* orderData) {
    stage = ST_DONE;

    pos_ptr = &pos;
    order_data = orderData ? *orderData : MainOrderData{};
    qs_in_check = false;

    tt = tt_move;
    killer_1 = killer1;
    killer_2 = killer2;
    counter = counter_move;

    has_tt = (tt != 0);
    has_k1 = (killer_1 != 0 && killer_1 != tt);
    has_k2 = (killer_2 != 0 && killer_2 != tt && killer_2 != killer_1);
    has_counter = (counter != 0 && counter != tt && counter != killer_1 && counter != killer_2);

    cur = goodCaptEnd = captEnd = quietEnd = badCaptCur = 0;
    stage = has_tt ? ST_TT : ST_GEN_CAPTURES;
}

void MovePicker::init_qsearch(const Position& pos,
    bool in_check,
    Move tt_move) {
    stage = ST_DONE;

    pos_ptr = &pos;
    order_data = MainOrderData{};
    qs_in_check = in_check;

    tt = tt_move;
    killer_1 = 0;
    killer_2 = 0;
    counter = 0;

    has_tt = (tt != 0);
    has_k1 = false;
    has_k2 = false;
    has_counter = false;

    cur = goodCaptEnd = captEnd = quietEnd = badCaptCur = 0;
    stage = has_tt ? ST_QS_TT : ST_QS_GEN;
}

Move MovePicker::next(bool skip_quiets) {
    if (!pos_ptr)
        return 0;

    for (;;) {
        switch (stage) {
        case ST_DONE:
            return 0;

        case ST_TT:
            stage = ST_GEN_CAPTURES;
            if (has_tt && pos_ptr->is_legal(tt)) {
                return tt;
            }
            continue;

        case ST_GEN_CAPTURES: {
            MoveList list;
            generate_captures(*pos_ptr, list);

            captEnd = 0;
            for (int i = 0; i < list.size; ++i) {
                const Move m = list.moves[i];
                if (!m || m == tt)
                    continue;

                moves[captEnd] = m;
                scores[captEnd] = score_main_move(*pos_ptr, m, order_data);
                captEnd++;
            }

            goodCaptEnd = 0;
            for (int i = 0; i < captEnd; ++i) {
                Move m = moves[i];
                bool is_good = true;

                if (is_capture(m)) {
                    const int see_margin = -movepick_capture_mvv_lva(*pos_ptr, m) / 32;
                    if (!movepick_see_ge(*pos_ptr, m, see_margin)) {
                        is_good = false;
                    }
                }

                if (is_good) {
                    if (i != goodCaptEnd) {
                        std::swap(moves[i], moves[goodCaptEnd]);
                        std::swap(scores[i], scores[goodCaptEnd]);
                    }
                    goodCaptEnd++;
                }
            }

            stage = ST_CAPTURES;
            cur = 0;
            continue;
        }

        case ST_CAPTURES: {
            while (cur < goodCaptEnd) {
                select_best(cur, goodCaptEnd);
                return moves[cur++];
            }
            stage = ST_KILLER1;
            continue;
        }

        case ST_KILLER1:
            stage = ST_KILLER2;
            if (has_k1 && pos_ptr->is_legal(killer_1) && !is_capture(killer_1) && !is_promotion(killer_1)) {
                return killer_1;
            }
            continue;

        case ST_KILLER2:
            stage = ST_COUNTER;
            if (has_k2 && pos_ptr->is_legal(killer_2) && !is_capture(killer_2) && !is_promotion(killer_2)) {
                return killer_2;
            }
            continue;

        case ST_COUNTER:
            stage = ST_GEN_QUIETS;
            if (has_counter && pos_ptr->is_legal(counter) && !is_capture(counter) && !is_promotion(counter)) {
                return counter;
            }
            continue;

        case ST_GEN_QUIETS: {
            if (skip_quiets) {
                stage = ST_BAD_CAPTURES;
                badCaptCur = goodCaptEnd;
                continue;
            }

            MoveList list;
            generate_quiets(*pos_ptr, list);

            Bitboard threatByLesser[PIECE_TYPE_NB] = { 0 };
            Color opp = ~pos_ptr->side_to_move();


            Bitboard pawnAttacks = pos_ptr->attacks_by(opp, PAWN);
            Bitboard knightAttacks = pos_ptr->attacks_by(opp, KNIGHT);
            Bitboard bishopAttacks = pos_ptr->attacks_by(opp, BISHOP);
            Bitboard rookAttacks = pos_ptr->attacks_by(opp, ROOK);
            Bitboard queenAttacks = pos_ptr->attacks_by(opp, QUEEN);


            threatByLesser[PAWN] = 0;
            threatByLesser[KNIGHT] = threatByLesser[BISHOP] = pawnAttacks;
            threatByLesser[ROOK] = pawnAttacks | knightAttacks | bishopAttacks;
            threatByLesser[QUEEN] = pawnAttacks | knightAttacks | bishopAttacks | rookAttacks;
            threatByLesser[KING] = pawnAttacks | knightAttacks | bishopAttacks | rookAttacks | queenAttacks;


            quietEnd = captEnd;
            for (int i = 0; i < list.size; ++i) {
                const Move m = list.moves[i];
                if (!m) continue;
                if (m == tt || m == killer_1 || m == killer_2 || m == counter) continue;
                if (is_capture(m) || is_promotion(m)) continue;

                int score = score_main_move(*pos_ptr, m, order_data);


                Square from = from_sq(m);
                Square to = to_sq(m);
                PieceType pt = piece_type(pos_ptr->piece_on(from));

                if (pt >= PAWN && pt <= KING) {
                    bool threatened_to = (threatByLesser[pt] & square_bb(to)) != 0;
                    bool threatened_from = (threatByLesser[pt] & square_bb(from)) != 0;


                    int v = threatened_to ? -19 : (threatened_from ? 20 : 0);
                    score += value_of(pt) * v;
                }

                moves[quietEnd] = m;
                scores[quietEnd] = score;
                quietEnd++;
            }

            stage = ST_QUIETS;
            cur = captEnd;
            continue;
        }

        case ST_QUIETS: {
            if (skip_quiets) {
                stage = ST_BAD_CAPTURES;
                badCaptCur = goodCaptEnd;
                continue;
            }

            while (cur < quietEnd) {
                select_best(cur, quietEnd);
                return moves[cur++];
            }
            stage = ST_BAD_CAPTURES;
            badCaptCur = goodCaptEnd;
            continue;
        }

        case ST_BAD_CAPTURES: {
            while (badCaptCur < captEnd) {
                select_best(badCaptCur, captEnd);
                return moves[badCaptCur++];
            }
            stage = ST_DONE;
            continue;
        }

        case ST_QS_TT:
            stage = ST_QS_GEN;
            if (has_tt
                && pos_ptr->is_legal(tt)
                && (qs_in_check || is_capture(tt) || is_promotion(tt))) {
                return tt;
            }
            continue;

        case ST_QS_GEN: {
            MoveList list;
            if (qs_in_check)
                generate_moves(*pos_ptr, list);
            else
                generate_captures(*pos_ptr, list);

            captEnd = 0;
            for (int i = 0; i < list.size; ++i) {
                const Move m = list.moves[i];
                if (!m || m == tt)
                    continue;

                int score = (is_capture(m) || is_promotion(m)) ? score_main_move(*pos_ptr, m, order_data) : 0;
                moves[captEnd] = m;
                scores[captEnd] = score;
                captEnd++;
            }

            stage = ST_QS_MOVES;
            cur = 0;
            continue;
        }

        case ST_QS_MOVES: {
            while (cur < captEnd) {
                select_best(cur, captEnd);
                return moves[cur++];
            }
            stage = ST_DONE;
            continue;
        }
        }
    }
}
