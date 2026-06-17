#include "thread.h"

#include "tt.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
    constexpr int THREAD_SCORE_SLACK = 50;

    bool is_decisive_mate_score(int score)
    {
        return std::abs(score) >= SEARCH_MATE_SCORE - MAX_PLY;
    }

    bool better_result(const SearchResult& candidate, const SearchResult& best)
    {
        if (candidate.best_move == 0)
            return false;

        if (best.best_move == 0)
            return true;

        const bool candidate_mate = is_decisive_mate_score(candidate.score);
        const bool best_mate = is_decisive_mate_score(best.score);
        if (candidate_mate != best_mate)
            return candidate.score > best.score;

        if (candidate.depth > best.depth && candidate.score >= best.score - THREAD_SCORE_SLACK)
            return true;

        if (candidate.depth == best.depth && candidate.score > best.score)
            return true;

        return false;
    }
}

SearchThreadPool::~SearchThreadPool()
{
    shutdown();
}

void SearchThreadPool::set_thread_count(int count)
{
    count = std::max(1, count);
    bool had_workers = false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        desired_threads_ = count;
        if (!workers_.empty() && static_cast<int>(workers_.size()) == count)
            return;
        had_workers = !workers_.empty();
    }

    if (!had_workers) {
        start_workers(count);
        return;
    }

    shutdown();
    start_workers(count);
}

int SearchThreadPool::thread_count() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return workers_.empty() ? desired_threads_ : static_cast<int>(workers_.size());
}

void SearchThreadPool::start_workers(int count)
{
    std::lock_guard<std::mutex> lock(mutex_);

    exiting_ = false;
    searching_ = false;
    active_workers_ = 0;
    results_.assign(static_cast<size_t>(count), SearchResult{});
    worker_nodes_ = std::make_unique<WorkerNodeCounter[]>(static_cast<size_t>(count));
    worker_node_count_ = count;
    for (int i = 0; i < count; ++i)
        worker_nodes_[i].nodes.store(0, std::memory_order_relaxed);
    workers_.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
        auto worker = std::make_unique<Worker>(i);
        worker->thread = std::thread([this, i] { worker_loop(i); });
        workers_.push_back(std::move(worker));
    }
}

void SearchThreadPool::shutdown()
{
    stop_and_wait();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (workers_.empty())
            return;

        exiting_ = true;
        ++job_generation_;
    }

    work_cv_.notify_all();

    for (auto& worker : workers_) {
        if (worker->thread.joinable())
            worker->thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        workers_.clear();
        results_.clear();
        worker_nodes_.reset();
        worker_node_count_ = 0;
        exiting_ = false;
        searching_ = false;
        active_workers_ = 0;
        on_complete_ = nullptr;
    }
}

void SearchThreadPool::start_search(const ThreadSearchJob& job, CompletionCallback on_complete)
{
    wait_for_idle();

    auto queued_job = std::make_unique<ThreadSearchJob>(job);
    queued_job->stop_epoch_baseline = current_stop_epoch();

    tt_new_search();

    bool needs_workers = false;
    int count = 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        needs_workers = workers_.empty();
        count = desired_threads_;
    }

    if (needs_workers)
        start_workers(count);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        current_job_ = *queued_job;
        results_.assign(workers_.size(), SearchResult{});
        for (int i = 0; i < worker_node_count_; ++i)
            worker_nodes_[i].nodes.store(0, std::memory_order_relaxed);
        active_workers_ = static_cast<int>(workers_.size());
        searching_ = active_workers_ > 0;
        on_complete_ = std::move(on_complete);
        ++job_generation_;
    }

    if (!searching_) {
        idle_cv_.notify_all();
        return;
    }

    work_cv_.notify_all();
}

void SearchThreadPool::stop_and_wait()
{
    if (!is_searching()) {
        wait_for_idle();
        return;
    }

    stop_search_now();
    wait_for_idle();
}

void SearchThreadPool::wait_for_idle()
{
    std::unique_lock<std::mutex> lock(mutex_);
    idle_cv_.wait(lock, [this] { return !searching_; });
}

void SearchThreadPool::clear_worker_state()
{
    const int count = thread_count();
    shutdown();
    set_thread_count(count);
}

bool SearchThreadPool::is_searching() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return searching_;
}

SearchResult SearchThreadPool::best_result_locked() const
{
    uint64_t total_nodes = 0;
    for (const SearchResult& result : results_)
        total_nodes += result.nodes;

    SearchResult best = results_.empty() ? SearchResult{} : results_[0];
    if (best.best_move == 0) {
        for (const SearchResult& result : results_) {
            if (better_result(result, best))
                best = result;
        }
    }

    best.nodes = total_nodes;
    return best;
}

uint64_t SearchThreadPool::total_worker_nodes(void* context)
{
    auto* pool = static_cast<SearchThreadPool*>(context);
    if (!pool || !pool->worker_nodes_)
        return 0;

    uint64_t total = 0;
    for (int i = 0; i < pool->worker_node_count_; ++i)
        total += pool->worker_nodes_[i].nodes.load(std::memory_order_relaxed);

    return total;
}

void SearchThreadPool::worker_loop(int index)
{
    uint64_t observed_generation = 0;

    while (true) {
        auto job = std::make_unique<ThreadSearchJob>();
        uint64_t my_generation = 0;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_cv_.wait(lock, [this, &observed_generation] {
                return exiting_ || (searching_ && job_generation_ != observed_generation);
                });

            if (exiting_)
                return;

            observed_generation = job_generation_;
            my_generation = observed_generation;
            *job = current_job_;
        }

        const bool helper_thread = index != 0;
        SearchResult result = search(job->pos,
            job->depth,
            job->movetime,
            job->time_left,
            job->increment,
            job->moves_to_go,
            job->infinite,
            job->nodes,
            job->move_overhead,
            false,
            job->stop_epoch_baseline,
            helper_thread,
            false,
            (worker_nodes_ && index >= 0 && index < worker_node_count_) ? &worker_nodes_[index].nodes : nullptr,
            &SearchThreadPool::total_worker_nodes,
            this);

        if (index == 0)
            stop_search_now();

        CompletionCallback callback;
        SearchResult best{};
        bool completed = false;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (my_generation == job_generation_ && searching_) {
                if (index >= 0 && index < static_cast<int>(results_.size()))
                    results_[static_cast<size_t>(index)] = result;

                --active_workers_;
                if (active_workers_ == 0) {
                    best = best_result_locked();
                    callback = std::move(on_complete_);
                    on_complete_ = nullptr;
                    searching_ = false;
                    completed = true;
                }
            }
        }

        if (completed) {
            if (callback)
                callback(best);
            idle_cv_.notify_all();
        }
    }
}
