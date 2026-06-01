#pragma once
#include <cstdint>
#include <chrono>

class TimeManager {
public:
    TimeManager();

    void reset();
    void set_limits(bool infinite,
        int move_time,
        int timeLeft,
        int inc,
        int movesToGo,
        int gamePly,
        int moveOverhead);

    void start();
    int elapsed() const;

    void update_after_iteration(int depth,
        uint32_t bestMove,
        int bestScore,
        uint64_t bestMoveNodes,
        uint64_t totalNodes,
        int rootMoveCount);

    bool should_stop() const;
    bool hard_stop() const;

    int optimum() const;
    int maximum() const;
    uint64_t poll_interval_mask() const;

private:
    void compute_time();
    void reset_iteration_state();

    bool infinite_mode;
    bool fixed_movetime;
    bool time_limited;

    int time_left;
    int increment;
    int moves_to_go;
    int game_ply;
    int move_overhead;

    int optimum_time;
    int maximum_time;

    int base_optimum_time;

    uint32_t previous_best_move;
    int last_best_move_depth;
    int previous_score;
    bool has_previous_score;
    double total_best_move_changes;
    double previous_time_reduction;
    int iter_scores[4];
    int iter_score_index;

    std::chrono::steady_clock::time_point start_time;
};
