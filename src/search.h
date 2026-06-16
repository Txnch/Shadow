#pragma once

#include <atomic>
#include <cstdint>
#include "move.h"
#include "position.h"


extern bool g_silent_search;
extern std::atomic<bool> g_uci_show_wdl;

inline constexpr int SEARCH_INF = 30000;
inline constexpr int SEARCH_MATE_SCORE = 29000;

struct SearchResult
{
    Move best_move;
    int  score;
    int  depth;
    uint64_t nodes;
};

// Eval Correction Tables
constexpr int CORR_SIZE = 16384;
constexpr int CORR_MASK = CORR_SIZE - 1;
constexpr int CORR_HISTORY_LIMIT = 8192;
constexpr int CORR_BONUS_LIMIT = CORR_HISTORY_LIMIT / 4;

struct SearchMoveBuffer {
    Move quiet_searched[MAX_MOVES]{};
    Piece quiet_pieces[MAX_MOVES]{};
    Move capture_searched[MAX_MOVES]{};
    Piece capture_attackers[MAX_MOVES]{};
    Piece capture_victims[MAX_MOVES]{};
};

static constexpr int SEARCH_MOVE_BUFFER_COUNT = MAX_PLY * 4;

struct SearchStack {
    Move current_move = 0;
    Piece moved_piece = NO_PIECE;
    int static_eval = -SEARCH_INF;
    bool tt_pv = false;
    Move excluded_move = 0;
    nnue::AccumulatorPair acc{};
    bool acc_valid = false;
};

struct RootMove {
    Move move = 0;
    int score = -SEARCH_INF;
    int window_score = -SEARCH_INF;
    int previous_score = -SEARCH_INF;
    int uci_score = -SEARCH_INF;
    bool lowerbound = false;
    bool upperbound = false;
    int seldepth = 0;
    uint64_t nodes = 0;
    Move pv[MAX_PLY]{};
    int pv_length = 0;
};

struct RootSearchContext {
    RootMove* root_moves = nullptr;
    int root_count = 0;
    int root_depth = 0;
    Move best_move = 0;
    int best_score = -SEARCH_INF;
    uint64_t best_move_nodes = 0;
};

struct SearchThreadState {
    Move killer_moves[2][MAX_PLY]{};
    int history_heuristic[64][64]{};
    Move countermove[64][64]{};
    int cont_history[PIECE_NB][64][PIECE_NB][64]{};
    int capture_history[16][64][16]{};

    int pawn_corr[2][CORR_SIZE]{};
    int non_pawn_corr[2][2][CORR_SIZE]{};
    int cont_corr[PIECE_NB][64]{};

    uint64_t local_stop_epoch = 0;
    bool stop_flag = false;
    uint64_t nodes_count = 0;
    uint64_t target_max_nodes = 0;
    bool target_nodes_soft = false;
    int seldepth = 0;
    int nmp_min_ply = 0;

    Move pv_table[MAX_PLY][MAX_PLY]{};
    int pv_length[MAX_PLY]{};

    int move_buffer_depth = 0;

    RootSearchContext* active_root_context = nullptr;
};

void stop_search_now();
uint64_t current_stop_epoch();
void clear_search_state_for_new_game();

SearchResult search(Position& pos,
    int max_depth,
    int movetime,
    int time_left,
    int increment,
    int moves_to_go,
    bool infinite,
    uint64_t max_nodes = 0,
    int move_overhead = 10,
    bool soft_node_limit = false,
    uint64_t stop_epoch_baseline = UINT64_MAX);
