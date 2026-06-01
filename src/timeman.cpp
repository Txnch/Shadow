#include "timeman.h"

#include <algorithm>
#include <cmath>

namespace {

    constexpr int DEFAULT_MOVE_OVERHEAD_MS = 10;
    constexpr int FIXED_MOVETIME_OVERHEAD_MS = 10;
    constexpr int MAX_MOVE_OVERHEAD_MS = 5000;

    static inline int clampi(int x, int lo, int hi) {
        return std::max(lo, std::min(x, hi));
    }

    static inline double clampd(double x, double lo, double hi) {
        return std::max(lo, std::min(x, hi));
    }

    static inline double interpolate(double x,
        double x0,
        double x1,
        double y0,
        double y1) {
        if (x1 == x0)
            return y0;

        const double t = (x - x0) / (x1 - x0);
        return y0 + t * (y1 - y0);
    }

} // namespace

TimeManager::TimeManager() {
    reset();
}

void TimeManager::reset_iteration_state() {
    previous_best_move = 0;
    last_best_move_depth = 0;
    previous_score = 0;
    has_previous_score = false;
    total_best_move_changes = 0.0;
    previous_time_reduction = 1.0;
    iter_score_index = 0;

    for (int& score : iter_scores)
        score = 0;
}

void TimeManager::reset() {
    infinite_mode = false;
    fixed_movetime = false;
    time_limited = false;

    time_left = 0;
    increment = 0;
    moves_to_go = 0;
    game_ply = 0;
    move_overhead = DEFAULT_MOVE_OVERHEAD_MS;

    optimum_time = 0;
    maximum_time = 0;

    base_optimum_time = 0;

    reset_iteration_state();
}

void TimeManager::set_limits(bool infinite,
    int move_time,
    int timeLeft,
    int inc,
    int movesToGo,
    int gamePly,
    int moveOverhead) {
    infinite_mode = infinite;
    fixed_movetime = false;
    time_limited = !infinite && (move_time > 0 || timeLeft > 0);
    game_ply = std::max(0, gamePly);
    move_overhead = clampi(moveOverhead, 0, MAX_MOVE_OVERHEAD_MS);

    reset_iteration_state();

    if (infinite_mode) {
        optimum_time = maximum_time = 0;
        base_optimum_time = 0;
        return;
    }

    time_left = std::max(0, timeLeft);
    increment = std::max(0, inc);
    moves_to_go = std::max(0, movesToGo);

    if (move_time > 0) {
        fixed_movetime = true;
        time_limited = true;

        const int margin = std::min(FIXED_MOVETIME_OVERHEAD_MS, std::max(0, move_time - 1));
        const int bound = std::max(1, move_time - margin);

        optimum_time = bound;
        maximum_time = bound;

        base_optimum_time = optimum_time;
        return;
    }

    compute_time();

    base_optimum_time = optimum_time;
}

void TimeManager::compute_time() {
    if (time_left <= 0) {
        optimum_time = maximum_time = 0;
        return;
    }

    const double time = double(std::max(1, time_left));
    const double inc = double(std::max(0, increment));
    const double overhead = double(std::max(0, move_overhead));

    int mtg = moves_to_go > 0 ? std::min(moves_to_go, 50) : 50;

    if (time < 1000.0)
        mtg = std::max(1, int(time * 0.05));

    const double time_left_adjusted = std::max(1.0, time + inc * double(mtg - 1) - overhead * double(mtg + 2));

    double opt_scale = 0.0;
    double max_scale = 1.0;

    if (moves_to_go == 0) {
        double original_time_adjust = 0.3272 * std::log10(time_left_adjusted) - 0.4141;
        original_time_adjust = std::max(0.05, original_time_adjust);

        const double log_time_sec = std::log10(std::max(1.0, time) / 1000.0);
        const double opt_constant = std::min(0.0029869 + 0.00033554 * log_time_sec, 0.004905);
        const double max_constant = std::max(3.3744 + 3.0608 * log_time_sec, 3.1441);

        opt_scale = std::min(0.012112 + std::pow(double(game_ply) + 3.22713, 0.46866) * opt_constant,
            0.19404 * time / time_left_adjusted) * original_time_adjust;
        max_scale = std::min(6.873, max_constant + double(game_ply) / 12.352);
    }
    else {
        opt_scale = std::min((0.88 + double(game_ply) / 116.4) / double(mtg),
            0.88 * time / time_left_adjusted);
        max_scale = 1.3 + 0.11 * double(mtg);
    }

    const double optimum = std::max(1.0, opt_scale * time_left_adjusted);
    const double hard_cap = 0.8097 * time - overhead;
    const double maximum = std::max(optimum, std::min(hard_cap, max_scale * optimum));

    const int safe_available = std::max(1, time_left - std::min(move_overhead, std::max(0, time_left - 1)));
    optimum_time = clampi(int(optimum), 1, safe_available);
    maximum_time = clampi(int(maximum), optimum_time, safe_available);
}

void TimeManager::update_after_iteration(int depth,
    uint32_t bestMove,
    int bestScore,
    uint64_t bestMoveNodes,
    uint64_t totalNodes,
    int rootMoveCount) {
    if (infinite_mode || fixed_movetime || !time_limited || base_optimum_time <= 0 || maximum_time <= 0)
        return;

    total_best_move_changes *= 0.5;

    if (previous_best_move == 0) {
        previous_best_move = bestMove;
        last_best_move_depth = depth;
    }
    else if (bestMove != previous_best_move) {
        previous_best_move = bestMove;
        last_best_move_depth = depth;
        total_best_move_changes += 1.0;
    }

    if (!has_previous_score) {
        previous_score = bestScore;
        for (int& score : iter_scores)
            score = bestScore;
        has_previous_score = true;
    }

    const int old_score = previous_score;
    const int older_score = iter_scores[iter_score_index];

    double falling_eval = (11.87 + 2.21 * double(old_score - bestScore)
        + double(older_score - bestScore)) / 100.0;
    falling_eval = clampd(falling_eval, 0.572, 1.708);

    const double stable_depth = double(std::max(0, depth - last_best_move_depth));
    const double time_reduction = clampd(interpolate(stable_depth, 5.0, 18.0, 0.65, 1.55), 0.65, 1.55);
    const double reduction = (1.48 + previous_time_reduction) / (2.157 * time_reduction);

    const double best_move_instability = 1.096 + 2.29 * total_best_move_changes;

    const uint64_t nodes_effort = bestMoveNodes * 100000ULL / std::max<uint64_t>(1, totalNodes);
    const double high_best_move_effort = clampd(
        interpolate(double(nodes_effort), 79219.0, 101822.0, 0.924, 0.710),
        0.710,
        0.924);

    double target = double(base_optimum_time) * falling_eval * reduction
        * best_move_instability * high_best_move_effort;

    if (rootMoveCount == 1)
        target = std::min(561.7, target);

    optimum_time = clampi(int(target), 1, maximum_time);

    previous_score = bestScore;
    iter_scores[iter_score_index] = bestScore;
    iter_score_index = (iter_score_index + 1) & 3;
    previous_time_reduction = time_reduction;
}

void TimeManager::start() {
    start_time = std::chrono::steady_clock::now();
}

int TimeManager::elapsed() const {
    auto now = std::chrono::steady_clock::now();
    return int(std::chrono::duration_cast<std::chrono::milliseconds>(
        now - start_time
    ).count());
}

bool TimeManager::should_stop() const {
    if (infinite_mode || !time_limited)
        return false;

    if (optimum_time <= 0)
        return false;

    return elapsed() >= optimum_time;
}

bool TimeManager::hard_stop() const {
    if (infinite_mode || !time_limited)
        return false;

    if (maximum_time <= 0)
        return false;

    return elapsed() >= maximum_time;
}

int TimeManager::optimum() const {
    return optimum_time;
}

int TimeManager::maximum() const {
    return maximum_time;
}

uint64_t TimeManager::poll_interval_mask() const {
    if (infinite_mode)
        return 4095ULL;

    const int budget = optimum_time > 0 ? optimum_time : maximum_time;

    if (budget <= 0)    return 1023ULL;
    if (budget <= 10)   return 7ULL;
    if (budget <= 25)   return 15ULL;
    if (budget <= 50)   return 31ULL;
    if (budget <= 100)  return 63ULL;
    if (budget <= 250)  return 127ULL;
    if (budget <= 1000) return 255ULL;
    if (budget <= 5000) return 511ULL;

    return 1023ULL;
}
