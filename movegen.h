#pragma once
#include "move.h"
#include "position.h"

struct MoveList {
    Move moves[MAX_MOVES];
    int size = 0;

    void clear() { size = 0; }

    void push(Move m) {
        if (size >= MAX_MOVES)
            return;
        moves[size++] = m;
    }
};

void generate_moves(const Position& pos, MoveList& list);
void generate_captures(const Position& pos, MoveList& list);
void generate_quiets(const Position& pos, MoveList& list);
