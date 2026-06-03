#pragma once

#include <cstdint>
#include "move.h"
#include "position.h"


extern bool g_silent_search;

struct SearchResult
{
    Move best_move;
    int  score;
    int  depth;
    uint64_t nodes;
};

void stop_search_now();
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
    bool soft_node_limit = false);
