#include "timeman.h"

#include <algorithm>
#include <cmath>

namespace {

    constexpr int DEFAULT_MOVE_OVERHEAD_MS = 10;
    constexpr int FIXED_MOVETIME_OVERHEAD_MS = 10;
    constexpr int MAX_MOVE_OVERHEAD_MS = 5000;
    constexpr int SCORE_STABILITY_MARGIN_CP = 10;
    constexpr int SCORE_STABILITY_MIN_DEPTH = 5;
    constexpr double SCORE_STABILITY_BASE = 1.159;
    constexpr double SCORE_STABILITY_MUL = 0.051;
    constexpr double SCORE_STABILITY_MIN = 0.813;

    static inline int clampi(int x, int lo, int hi) {
        return std::max(lo, std::min(x, hi));
    }

    static inline double clampd(double x, double lo, double hi) {
        return std::max(lo, std::min(x, hi));
    }

} // namespace

TimeManager::TimeManager() {
    reset();
}

void TimeManager::reset_iteration_state() {
    previous_best_move = 0;
    previous_score = 0;
    completed_depth = 0;
    pv_stability = 0;
    score_stability = 0;
    has_previous_score = false;
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

    const int default_mtg = increment > 0 ? 35 : 45;
    const int mtg = (moves_to_go > 0 ? moves_to_go : default_mtg) + 5;
    const int soft = (time_left - increment) / std::max(1, mtg) + increment * 3 / 4;
    const int hard = time_left / 2;

    base_optimum_time = std::max(1, soft);
    optimum_time = base_optimum_time;
    maximum_time = std::max(1, hard);
}

void TimeManager::update_after_iteration(int depth,
    uint32_t bestMove,
    int bestScore,
    uint64_t bestMoveNodes,
    uint64_t totalNodes,
    int rootMoveCount) {
    if (infinite_mode || fixed_movetime || !time_limited || base_optimum_time <= 0 || maximum_time <= 0)
        return;

    (void)rootMoveCount;

    completed_depth = depth;

    const bool score_is_stable =
        has_previous_score && std::abs(bestScore - previous_score) <= SCORE_STABILITY_MARGIN_CP;

    if (depth < 4) {
        previous_best_move = bestMove;
        previous_score = bestScore;
        has_previous_score = true;
        return;
    }

    if (previous_best_move != 0 && bestMove == previous_best_move) {
        pv_stability = std::min(pv_stability + 1, 10);
    }
    else {
        pv_stability = 0;
    }

    previous_best_move = bestMove;

    if (score_is_stable) {
        ++score_stability;
    }
    else {
        score_stability = 0;
    }

    previous_score = bestScore;
    has_previous_score = true;

    const double nodes_ratio = clampd(
        double(bestMoveNodes) / double(std::max<uint64_t>(1, totalNodes)),
        0.0,
        1.0);

    double target = double(base_optimum_time);
    target *= 2.0 - 1.5 * nodes_ratio;
    target *= 1.25 - 0.05 * double(pv_stability);

    if (depth >= SCORE_STABILITY_MIN_DEPTH) {
        const double score_factor = std::max(
            SCORE_STABILITY_BASE - SCORE_STABILITY_MUL * double(score_stability),
            SCORE_STABILITY_MIN);
        target *= score_factor;
    }

    target = std::max(target, double(base_optimum_time) * 0.75);

    optimum_time = std::max(1, int(target));
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

    if (!fixed_movetime && completed_depth < 4)
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
    return 4095ULL;
}
