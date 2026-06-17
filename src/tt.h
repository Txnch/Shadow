#pragma once

#include <cstdint>

#include "move.h"

enum TTFlag { TT_EXACT, TT_ALPHA, TT_BETA };

struct TTEntry {
    uint64_t key;
    int depth;
    int score;
    TTFlag flag;
    Move best_move;
    int static_eval;
    uint8_t gen;
};

struct TTContext;

void tt_clear();
void tt_new_search();
TTEntry* tt_probe(uint64_t key);
void tt_store(uint64_t key, int depth, int score, TTFlag flag, Move best_move, int static_eval);
void tt_prefetch(uint64_t key);
int tt_hashfull();

bool tt_resize_mb(int mb);
int tt_hash_mb();

TTContext* tt_create_context(int mb);
void tt_destroy_context(TTContext* context);
void tt_bind_context(TTContext* context);
TTContext* tt_current_context();
