#pragma once

#include "position.h"

namespace wdl {

struct WDL {
    int win;
    int draw;
    int loss;
};

int material(const Position& pos);
int normalize_score(int score, const Position& pos);
WDL model(int score, const Position& pos);

}
