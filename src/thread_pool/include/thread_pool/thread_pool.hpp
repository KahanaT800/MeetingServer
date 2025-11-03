#pragma once

#include "thread_pool/fwd.hpp"
#include "mpmc/blocking_queue_adapter.hpp"
#include "thread_pool/config.hpp"
#include "logger.hpp"

#include <thread>
#include <future>
#include <vector>
#include <atomic>
#include <tuple>
#include <type_traits>
#include <future>
#include <functional>
#include <exception>

namespace thread_pool {
class ThreadPool {
public:
    explicit ThreadPool(std::size_t threads_count, std::size_t queue_cap = 1024);
    explicit ThreadPool(const ThreadPoolConfig& cfg);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    void Start();
    void Stop(StopMode mode = StopMode::Graceful);
    void ShutDown(ShutDownOption opt = ShutDownOption::Graceful, 
        std::chrono::milliseconds timeout = std::chrono::seconds(30));

    void Post(std::function<void()> f);
    template <typename Func, typename... Args>
    auto Submit(Func&& f, Args&&... args) -> std::future<std::invoke_result_t<Func, Args...>>;

    // Batch submission APIs
    template <typename Iterator>
    std::size_t PostBatch(Iterator begin, Iterator end);
    
    template <typename Func>
    std::size_t PostBatch(Func generator, std::size_t count);

    // Policy
    QueueFullPolicy GetQueueFullPolicy() const noexcept;
    void SetQueueFullPolicy(QueueFullPolicy policy) noexcept;

    void Pause() noexcept;
    void Resume() noexcept;
    bool Paused() const noexcept;

    // State queries
    bool Running() const noexcept;
    std::size_t Pending() const noexcept;
    std::size_t ActiveTasks() const noexcept;
    PoolState State() const noexcept;

    std::size_t DiscardedTasks() const noexcept;
    std::size_t OverwrittedTasks() const noexcept;
    std::size_t PausedWait() const noexcept;

    // Dynamic thread management
    void TriggerLoadCheck();  // Manually trigger load balancer

    std::size_t CurrentThreads() const noexcept;  // Current live worker threads
    std::size_t ActiveThreads() const noexcept;   // Active (busy) worker threads
    
    // Statistics API
    Statistics GetStatistics() const noexcept;
    void ResetStatistics() noexcept;
private:
    struct WorkerSlot {
        std::thread                           thread;              // worker object
        std::atomic<bool>                     should_exit{false};  // shrink/shutdown indicator
        std::atomic<bool>                     idle{true};          // worker idle state
        std::atomic<std::uint64_t>            idle_nums{0};        // consecutive idle count
        std::chrono::steady_clock::time_point last_active{};       // last time a task executed
    };

    struct ExitTask final : TaskBase {
        explicit ExitTask(WorkerSlot* target) noexcept : slot(target) {}
        void Execute() noexcept override {}
        void Cancel(std::exception_ptr) noexcept override {}
        WorkerSlot* slot{nullptr};
    };

    class WorkerCounterHelper {
    public:
        WorkerCounterHelper(ThreadPool& pool, WorkerSlot& slot) noexcept;
        ~WorkerCounterHelper() noexcept;
        WorkerCounterHelper(const WorkerCounterHelper&) = delete;
        WorkerCounterHelper& operator=(const WorkerCounterHelper&) = delete;
        void TaskOn();
        void TaskOff() noexcept;
    private:
        ThreadPool& pool_;
        WorkerSlot& slot_;
        std::atomic<bool> normal_end_{false};
    };
    
    // Submission counters
    void SubmitOn() noexcept;
    void SubmitOff() noexcept;

    void WorkerLoop(WorkerSlot* slot);
    void SetState(PoolState new_state) noexcept;

    template <class R>
    static std::future<R> BrokenFuture(std::exception_ptr eptr);
private:
    std::atomic<PoolState> state_;
    BlockingQueueAdapter<TaskPtr> queue_;
    std::vector<std::unique_ptr<WorkerSlot>> workers_;
    std::atomic<QueueFullPolicy> policy_;

    // Dynamic thread management
    mutable                 std::mutex workers_mu_;  // protects workers_ container
    std::thread             load_balancer_;          // balancer thread
    std::atomic<bool>       balancer_stop_{true};    // balancer exit flag
    std::atomic<bool>       balancer_kick_{false};   // manual wake flag
    std::condition_variable load_cv_;                // balancer loop wake-up
    std::mutex              load_cv_mu_;             // load-balancing mutex

    // State management
    std::mutex pause_mtx_;  // pause lock
    std::condition_variable pause_cv_;
    mutable std::mutex drain_mtx_;  // drain lock
    std::condition_variable drain_cv_;
    mutable std::mutex submit_mtx_;  // submission lock
    std::condition_variable submit_cv_;

    // Dynamic thread management parameters
    std::size_t               core_threads_{0};            // core thread count
    std::size_t               max_threads_{0};             // maximum thread count
    std::chrono::milliseconds load_check_interval_{0};     // load balancer sampling interval
    std::chrono::milliseconds keep_alive_{0};              // idle thread keep-alive time
    double                    scale_up_threshold_{0.0};    // busy ratio upper bound (scale up if exceeded)
    double                    scale_down_threshold_{0.0};  // busy ratio lower bound (scale down if below)
    std::size_t               pending_hi_{0};              // pending threshold (upper)
    std::size_t               pending_low_{0};             // pending threshold (lower)
    std::size_t               debounce_hits_{0};           // debounce hit count
    std::chrono::milliseconds cooldown_{0};                // cooldown after capacity change

    // Dynamic thread management interfaces
    void                     LaunchLoadBalancer();                                         // background balancer thread
    void                     StopLoadBalancer();                                           // stop and join balancer thread
    void                     LoadBalancerLoop();                                           // periodically sample load and adjust capacity
    void                     CreateWorkerUnlocked();                                       // create WorkerSlot and run WorkerLoop
    std::vector<WorkerSlot*> ScheduleShrinkUnlocked(std::size_t count);                    // mark workers to retire
    void                     EnqueueExitSignals(const std::vector<WorkerSlot*>& targets);  // enqueue directed exit tasks
    void                     RetireWorkerUnlocked(WorkerSlot& slot);                       // retire worker
private:
    // Task state
    std::atomic<size_t>      active_tasks_{0};  // active tasks
    std::atomic<std::size_t> submit_ing_{0};    // submissions in progress

    // Statistics
    std::atomic<std::size_t> total_submitted_{0};  // total tasks submitted successfully
    std::atomic<std::size_t> total_completed_{0};  // total tasks executed successfully
    std::atomic<std::size_t> total_failed_{0};     // total tasks failed during execution
    std::atomic<std::size_t> total_cancelled_{0};  // total tasks cancelled
    std::atomic<std::size_t> total_rejected_{0};   // total tasks rejected on submit

    std::atomic<std::size_t> total_exec_time_ns_{0};  // total execution time (ns)

    // pending is maintained by queue_
    std::atomic<double> busy_ratio_{0.0};     // busy thread ratio
    std::atomic<double> pending_ratio_{0.0};  // queue utilization ratio

    std::atomic<std::size_t> current_threads_{0};          // current running worker count
    std::atomic<std::size_t> active_threads_{0};           // workers currently executing tasks
    std::atomic<std::size_t> peak_threads_{0};             // peak worker count
    std::atomic<std::size_t> total_threads_created_{0};    // total workers created
    std::atomic<std::size_t> total_threads_destroyed_{0};  // total workers destroyed

    std::atomic<std::size_t> discard_cnt_{0};      // discarded task count
    std::atomic<std::size_t> overwrite_cnt_{0};    // overwritten task count
    std::atomic<std::size_t> paused_wait_cnt_{0};  // times waited due to pause

    void RecordTaskComplete(const TaskBase& task, std::chrono::steady_clock::duration duration) noexcept;
    void RecordTaskCancel() noexcept;
    void RecordTaskRejected() noexcept;
};

// Batch submission from an iterator range (uses lightweight SimpleTask)
template <typename Iterator>
inline std::size_t ThreadPool::PostBatch(Iterator begin, Iterator end) {
    if (State() != PoolState::RUNNING) {
        return 0;
    }

    std::vector<TaskPtr> tasks;
    tasks.reserve(std::distance(begin, end));
    
    for (auto it = begin; it != end; ++it) {
        // Use lightweight SimpleTask to avoid future overhead
        auto task = std::make_unique<SimpleTask>(
            typename SimpleTask::Func([f = *it]() mutable { f(); })
        );
        tasks.push_back(std::move(task));
    }

    const auto count = queue_.TryPushBatch(tasks.begin(), tasks.end());
    total_submitted_.fetch_add(count, std::memory_order_relaxed);
    
    return count;
}

// Batch submission using a generator function (uses lightweight SimpleTask)
template <typename Func>
inline std::size_t ThreadPool::PostBatch(Func generator, std::size_t count) {
    if (State() != PoolState::RUNNING) {
        return 0;
    }

    std::vector<TaskPtr> tasks;
    tasks.reserve(count);
    
    for (std::size_t i = 0; i < count; ++i) {
        auto f = generator(i);
        // Use lightweight SimpleTask to avoid future overhead
        auto task = std::make_unique<SimpleTask>(
            typename SimpleTask::Func(std::move(f))
        );
        tasks.push_back(std::move(task));
    }

    const auto pushed = queue_.TryPushBatch(tasks.begin(), tasks.end());
    total_submitted_.fetch_add(pushed, std::memory_order_relaxed);
    
    return pushed;
}

template <class R>
inline std::future<R> ThreadPool::BrokenFuture(std::exception_ptr eptr) {
    std::promise<R> promise; 
    promise.set_exception(std::move(eptr)); 
    return promise.get_future();
}

template <typename Func, typename... Args>
auto ThreadPool::Submit(Func&& f, Args&&... args) -> std::future<std::invoke_result_t<Func, Args...>> {
    using Return = std::invoke_result_t<Func, Args...>;

    // Package into a closure
    auto bound = [ff = std::forward<Func>(f), 
                    tup = std::make_tuple(std::forward<Args>(args)...)]() mutable -> Return 
    { 
        return std::apply(std::move(ff), std::move(tup)); 
    }; 

    auto task_ptr = std::make_unique<FutureTask<Return>>(
        typename FutureTask<Return>::Func(std::move(bound)) 
        // typename FutureTask<Return>::Func = std::function<Return()> (type erasure)
    );
    std::future<Return> fut;
    try {
        fut = task_ptr->GetFuture();
    } catch (const std::exception& ex) {
        TP_LOG_ERROR("Submit failed to acquire future: {}", ex.what());
        throw;
    } catch (...) {
        TP_LOG_ERROR("Submit failed to acquire future: unknown exception");
        throw;
    }

    // RAII helper for submission counting
    struct SubmitHelper {
        ThreadPool* pool;
        explicit SubmitHelper(ThreadPool* p) : pool(p) {
            pool->SubmitOn();
        }
        // decrement on destruction
        ~SubmitHelper() {
            if (pool) {
                pool->SubmitOff();
            }
        }
        SubmitHelper(const SubmitHelper&) = delete;
        SubmitHelper& operator=(const SubmitHelper&) = delete;
    } helper(this);

    bool waited_in_pause = false;
    for (;;) {
        PoolState s = state_.load(std::memory_order_acquire);
        if (s == PoolState::RUNNING) {
            if (waited_in_pause) {
                TP_LOG_DEBUG("Submit resumed after pause; proceeding with enqueue");
            }
            break;
        }
        if (s == PoolState::PAUSED) {
            TP_LOG_DEBUG("Submit blocked: pool paused");
            std::unique_lock<std::mutex> lk(pause_mtx_);
            paused_wait_cnt_.fetch_add(1, std::memory_order_relaxed);
            pause_cv_.wait(lk, [this] {
                auto t = state_.load(std::memory_order_acquire);
                return t != PoolState::PAUSED;
            });
            waited_in_pause = true;
            // Check wake reason (e.g., Stop)
            continue;
        }

        if (waited_in_pause && s == PoolState::SHUTTING_DOWN) {
            TP_LOG_INFO("Submit allowed during shutdown because task waited before pause");
            break; // Submission waited during pause before shutdown
        }
        if (waited_in_pause && s == PoolState::FORCE_STOPPING) {
            RecordTaskCancel(); // task cancelled
            // force stop
            auto eptr = std::make_exception_ptr(
                std::runtime_error("force stopped")
            );
            task_ptr->Cancel(eptr);
            TP_LOG_ERROR("Submit cancelled: pool force stopping; pending={}", Pending());
            return BrokenFuture<Return>(eptr);
        }
        RecordTaskRejected(); // task rejected
        TP_LOG_ERROR("Submit rejected: pool state={} (expected RUNNING)", s);
        throw std::runtime_error("ThreadPool::Submit: pool is not RUNNING");
    }

    // Dispatch by queue policy
    const auto policy = policy_.load(std::memory_order_relaxed);
    switch (policy) {
        case QueueFullPolicy::Block: {
            if (!queue_.WaitPush(std::move(task_ptr))) {
                RecordTaskRejected(); // task rejected
                auto eptr = std::make_exception_ptr(
                    std::runtime_error("ThreadPool::Submit: queue closed")
                );
                TP_LOG_WARN("Submit failed (policy=Block): queue closed, pending={}, state={}",
                            Pending(), State());
                return BrokenFuture<Return>(eptr);
            }
            total_submitted_.fetch_add(1, std::memory_order_relaxed); // successfully enqueued
            TP_LOG_TRACE("Submit succeeded (policy=Block): pending={} queue_cap={}",
                         Pending(), queue_.Capacity());
            return fut;
        }

        case QueueFullPolicy::Discard: {
             if (!queue_.TryPush(std::move(task_ptr))) {
                RecordTaskRejected(); // task rejected
                discard_cnt_.fetch_add(1, std::memory_order_relaxed);
                auto eptr = std::make_exception_ptr(
                    std::runtime_error("ThreadPool::Submit: discarded")
                );
                TP_LOG_WARN("Submit discarded (policy=Discard): pending={} discard_cnt={}",
                            Pending(), discard_cnt_.load(std::memory_order_relaxed));
                return BrokenFuture<Return>(eptr);
            }
            total_submitted_.fetch_add(1, std::memory_order_relaxed); // successfully enqueued
            TP_LOG_DEBUG("Submit accepted (policy=Discard): pending={} discard_cnt={}",
                         Pending(), discard_cnt_.load(std::memory_order_relaxed));
            return fut;
        }
        
        case QueueFullPolicy::Overwrite: {
            TaskPtr overwritten;
            bool pushed = queue_.OverwritePush(std::move(task_ptr), &overwritten);
            if (overwritten) {
                overwritten->Cancel(std::make_exception_ptr(
                    std::runtime_error("ThreadPool::Submit: overwritten")
                ));
                RecordTaskCancel();
                overwrite_cnt_.fetch_add(1, std::memory_order_relaxed);
                TP_LOG_DEBUG("Submit overwrote existing task (policy=Overwrite): overwrite_cnt={}",
                             overwrite_cnt_.load(std::memory_order_relaxed));
            }
            if (!pushed) {
                RecordTaskRejected(); // task rejected
                discard_cnt_.fetch_add(1, std::memory_order_relaxed);
                auto eptr = std::make_exception_ptr(
                    std::runtime_error("ThreadPool::Submit: overwrite failed"));
                TP_LOG_WARN("Submit failed (policy=Overwrite): queue could not accept task, pending={}, discard_cnt={}",
                            Pending(), discard_cnt_.load(std::memory_order_relaxed));
                return BrokenFuture<Return>(eptr);
            }
            total_submitted_.fetch_add(1, std::memory_order_relaxed); // successfully enqueued
            TP_LOG_TRACE("Submit enqueued (policy=Overwrite): pending={} overwrite_cnt={}",
                         Pending(), overwrite_cnt_.load(std::memory_order_relaxed));
            return fut;
        }
    }

    // Should not reach here
    auto eptr = std::make_exception_ptr(
        std::runtime_error("ThreadPool::Submit: unknown policy")
    );
    return BrokenFuture<Return>(eptr);
}
}
