#pragma once

#include "position.h"
#include "search.h"

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

struct ThreadSearchJob {
    Position pos;
    int depth = 64;
    int movetime = -1;
    int time_left = 0;
    int increment = 0;
    int moves_to_go = 0;
    bool infinite = false;
    uint64_t nodes = 0;
    int move_overhead = 10;
    uint64_t stop_epoch_baseline = UINT64_MAX;
};

class SearchThreadPool {
public:
    using CompletionCallback = std::function<void(const SearchResult&)>;

    SearchThreadPool() = default;
    ~SearchThreadPool();

    SearchThreadPool(const SearchThreadPool&) = delete;
    SearchThreadPool& operator=(const SearchThreadPool&) = delete;

    void set_thread_count(int count);
    int thread_count() const;

    void start_search(const ThreadSearchJob& job, CompletionCallback on_complete);
    void stop_and_wait();
    void wait_for_idle();
    void clear_worker_state();
    void shutdown();

    bool is_searching() const;

private:
    struct Worker {
        explicit Worker(int worker_index) : index(worker_index) {}

        int index = 0;
        std::thread thread;
    };

    struct alignas(64) WorkerNodeCounter {
        std::atomic<uint64_t> nodes{ 0 };
    };

    mutable std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable idle_cv_;

    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<SearchResult> results_;
    std::unique_ptr<WorkerNodeCounter[]> worker_nodes_;
    int worker_node_count_ = 0;

    ThreadSearchJob current_job_{};
    CompletionCallback on_complete_{};

    bool exiting_ = false;
    bool searching_ = false;
    uint64_t job_generation_ = 0;
    int active_workers_ = 0;
    int desired_threads_ = 1;

    void start_workers(int count);
    void worker_loop(int index);
    SearchResult best_result_locked() const;
    static uint64_t total_worker_nodes(void* context);
};
