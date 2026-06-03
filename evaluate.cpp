#include "nnue.h"

int evaluate(const Position& pos)
{
    return nnue::is_ready() ? nnue::evaluate(pos) : 0;
}
