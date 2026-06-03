#pragma once

#include "move.h"

class Position;

int movepick_capture_mvv_lva(const Position& pos, Move m);
bool movepick_see_ge(const Position& pos, Move m, int threshold);

class MovePicker {
public:
    static constexpr int CONTHIST1_WEIGHT = 124;
    static constexpr int CONTHIST2_WEIGHT = 127;
    static constexpr int CONTHIST4_WEIGHT = 77;

    struct MainOrderData {
        const int (*history)[64] = nullptr;
        const int (*capture_history)[64][16] = nullptr;
        const int (*cont1)[64] = nullptr;
        const int (*cont2)[64] = nullptr;
        const int (*cont4)[64] = nullptr;
    };

    MovePicker();

    static int score_main_move(const Position& pos, Move m, const MainOrderData& order_data);

    void init_main(const Position& pos,
        Move tt_move,
        Move killer1,
        Move killer2,
        Move counter_move,
        const MainOrderData* order_data);


    void init_qsearch(const Position& pos,
        bool in_check,
        Move tt_move);

    Move next(bool skip_quiets = false);

private:
    enum Stage {
        ST_DONE,
        ST_TT,
        ST_GEN_CAPTURES,
        ST_CAPTURES,
        ST_KILLER1,
        ST_KILLER2,
        ST_COUNTER,
        ST_GEN_QUIETS,
        ST_QUIETS,
        ST_BAD_CAPTURES,
        ST_QS_TT,
        ST_QS_GEN,
        ST_QS_MOVES,
    };


    void select_best(int begin, int end);

    Stage stage = ST_DONE;

    const Position* pos_ptr = nullptr;
    MainOrderData order_data{};
    bool qs_in_check = false;

    Move tt = 0;
    Move killer_1 = 0;
    Move killer_2 = 0;
    Move counter = 0;

    bool has_tt = false;
    bool has_k1 = false;
    bool has_k2 = false;
    bool has_counter = false;


    Move moves[MAX_MOVES];
    int  scores[MAX_MOVES];


    int cur = 0;
    int goodCaptEnd = 0;
    int captEnd = 0;
    int quietEnd = 0;
    int badCaptCur = 0;
};
