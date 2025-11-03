#include "thread_pool/fwd.hpp"
#include "thread_pool/thread_pool.hpp"
#include "mpmc/blocking_queue_adapter.hpp"
#include "logger.hpp"

#include <functional>
#include <stdexcept>
#include <cassert>
#include <utility>
#include <chrono>
#include <string>

namespace thread_pool {
namespace {

template <typename Duration>
long long ToMilliseconds(Duration duration) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

}

ThreadPool::ThreadPool(std::size_t threads_count, std::size_t queue_cap) 
    : state_(PoolState::CREATED)
    , queue_(queue_cap)
    , workers_()
    , policy_(QueueFullPolicy::Block)
{
    core_threads_         = std::max<std::size_t>(1, threads_count);  // Default core threads equals initial value
    max_threads_          = core_threads_;                            // If not configured, do not scale above core
    load_check_interval_  = std::chrono::milliseconds{100};           // Load balancer sampling interval
    keep_alive_           = std::chrono::milliseconds{5000};          // Idle thread keep-alive time
    scale_up_threshold_   = 0.75;                                     // Busy ratio upper bound; scale up when exceeded
    scale_down_threshold_ = 0.25;                                     // Busy ratio lower bound; scale down when below
    pending_hi_           = queue_cap / 2;                            // Pending threshold (upper)
    pending_low_          = std::max<std::size_t>(1, queue_cap / 8);  // Pending threshold (lower)
    debounce_hits_        = 3;                                        // Debounce hits for scale up/down
    cooldown_             = std::chrono::milliseconds{500};           // Cooldown after capacity change
    const auto policy = policy_.load(std::memory_order_relaxed);

    TP_LOG_DEBUG("ThreadPool constructed (direct): core_threads={} max_threads={} queue_cap={} policy={}",
                 core_threads_, max_threads_, queue_.Capacity(), policy);
}

ThreadPool::ThreadPool(const ThreadPoolConfig& cfg)
    : state_(PoolState::CREATED)
    , queue_(cfg.queue_cap)
    , workers_()
    , policy_(cfg.queue_policy)
{
    core_threads_         = std::max<std::size_t>(1, cfg.core_threads);   // Default core threads equals configured value
    max_threads_          = std::max(core_threads_, cfg.max_threads);     // Ensure max >= core
    load_check_interval_  = cfg.load_check_interval;                      // Load balancer sampling interval
    keep_alive_           = cfg.keep_alive;                               // Idle thread keep-alive time
    scale_up_threshold_   = cfg.scale_up_threshold;                       // Busy ratio upper bound; scale up when exceeded
    scale_down_threshold_ = cfg.scale_down_threshold;                     // Busy ratio lower bound; scale down when below
    pending_hi_           = cfg.pending_hi;                               // Pending threshold (upper)
    pending_low_          = std::min(cfg.pending_hi, cfg.pending_low);    // Pending threshold (lower)
    debounce_hits_        = std::max<std::size_t>(1, cfg.debounce_hits);  // Debounce hits
    cooldown_             = cfg.cooldown;                                 // Cooldown after capacity change
    const auto policy = policy_.load(std::memory_order_relaxed);
    
    TP_LOG_DEBUG("ThreadPool constructed (config): core_threads={} max_threads={} queue_cap={} policy={}",
                 core_threads_, max_threads_, queue_.Capacity(), policy);
}

ThreadPool::~ThreadPool () {
    // Force a graceful stop in destructor
    if (State() != PoolState::STOPPED) {
        try {
            Stop(StopMode::Graceful);
        } catch (...) {}
    }
}

void ThreadPool::Start() {
    TP_PERF_SCOPE("ThreadPool::Start");
    PoolState expected = PoolState::CREATED;
    if (!state_.compare_exchange_strong(expected, PoolState::RUNNING
                                                , std::memory_order_acq_rel
                                                , std::memory_order_acquire)) 
    {
        TP_LOG_WARN("ThreadPool start ignored: state={}", expected);
        return;
    }
    const auto policy = policy_.load(std::memory_order_relaxed);
    TP_LOG_INFO("ThreadPool starting: core_threads={} max_threads={} queue_cap={} policy={} load_interval={}ms keep_alive={}ms",
                core_threads_, max_threads_, queue_.Capacity(), policy,
                ToMilliseconds(load_check_interval_), ToMilliseconds(keep_alive_));
    {
        std::lock_guard<std::mutex> lk(workers_mu_);
        workers_.reserve(max_threads_); // Reserve up to max threads to avoid frequent reallocation
        current_threads_.store(0, std::memory_order_relaxed);
        active_threads_.store(0, std::memory_order_relaxed);
        for  (std::size_t i = 0; i < core_threads_; ++i) {
            CreateWorkerUnlocked();
        }
    }
    balancer_stop_.store(false, std::memory_order_release); // Enable dynamic load balancing
    LaunchLoadBalancer();
    const auto current_policy = policy_.load(std::memory_order_relaxed);
    TP_LOG_INFO("ThreadPool started with {} workers (policy={}, pending_hi={}, pending_low={})",
                current_threads_.load(std::memory_order_relaxed),
                current_policy,
                pending_hi_, pending_low_);
}

void ThreadPool::Stop(StopMode mode) {
    TP_PERF_SCOPE("ThreadPool::Stop");
    const auto current_state = state_.load(std::memory_order_acquire);
    TP_LOG_INFO("ThreadPool stop requested: mode={} state={}", mode, current_state);
    const bool graceful = (mode == StopMode::Graceful);
    // State transition
    for (;;) {
        PoolState s = state_.load(std::memory_order_acquire);
        if (s == PoolState::STOPPED) {
            break;
        }

        PoolState target = s;
        switch (s) {
        case PoolState::CREATED:
            target = PoolState::STOPPED;
            break;
        case PoolState::RUNNING:
        case PoolState::PAUSED:
            target = graceful ? PoolState::SHUTTING_DOWN : PoolState::FORCE_STOPPING;
            break;
        case PoolState::SHUTTING_DOWN:
            if (!graceful) {
                target = PoolState::FORCE_STOPPING;
            }
            break;
        case PoolState::FORCE_STOPPING:
            break;
        default:
            break;
        }

        if (target == s) {
            break;
        }
        if (state_.compare_exchange_strong(
                s, target, std::memory_order_acq_rel, std::memory_order_acquire)) {
            TP_LOG_DEBUG("ThreadPool stop state transition: {} -> {}", s, target);
            break;
        }
    }

    // Wake all paused producers/consumers
    {
        std::lock_guard<std::mutex> lk(pause_mtx_);
        pause_cv_.notify_all();
    }

    // Shutdown: graceful / force
    PoolState cur = state_.load(std::memory_order_acquire);
    TP_LOG_DEBUG("ThreadPool stop entering phase {}", cur);

    if (cur == PoolState::SHUTTING_DOWN) {
        TP_LOG_INFO("ThreadPool graceful shutdown: waiting for {} submissions in-flight", submit_ing_.load(std::memory_order_acquire));
        // Drain in-flight submissions
        {
            std::unique_lock<std::mutex> lk(submit_mtx_);
            submit_cv_.wait(lk, [this] {
                return submit_ing_.load(std::memory_order_acquire) == 0;
            });
        }
        TP_LOG_INFO("ThreadPool submissions drained, waiting for {} pending / {} active tasks",
                    Pending(), ActiveTasks());
        // All submissions done; wait for execution to complete
        {
            std::unique_lock<std::mutex> lk(drain_mtx_);
            drain_cv_.wait(lk, [this] {
                return Pending() == 0 && ActiveTasks() == 0;
            });
        }
        queue_.Close();
        TP_LOG_INFO("ThreadPool queue closed after graceful drain");
    } else if (cur == PoolState::FORCE_STOPPING) {
        const auto pending = Pending();
        TP_LOG_WARN("ThreadPool force stop: cancelling {} pending tasks", pending);
        // Force clear the queue
        queue_.Clear([&](TaskPtr& t) {
            if (t) {
                t->Cancel(std::make_exception_ptr(std::runtime_error("force stopped")));
                RecordTaskCancel();
            }
        });
        queue_.Close();
        TP_LOG_WARN("ThreadPool queue cleared; {} tasks marked cancelled", pending);
    } else if (cur == PoolState::STOPPED) {
        TP_LOG_DEBUG("ThreadPool already stopped");
        return;
    }

    // Stop dynamic load balancing
    StopLoadBalancer();

    // Join all worker threads
    std::vector<std::unique_ptr<WorkerSlot>> to_join;
    {
        std::lock_guard<std::mutex> lk(workers_mu_);
        to_join.swap(workers_); // Take ownership of all worker slots
    }
    const auto self = std::this_thread::get_id(); // Avoid self-join deadlock
    for (auto& slot : to_join) {
        if (slot && slot->thread.joinable() && slot->thread.get_id() != self) {
            slot->thread.join();
        }
    }

    // Mark as stopped
    state_.store(PoolState::STOPPED, std::memory_order_release);
    TP_LOG_INFO("ThreadPool stopped; workers joined={}, pending={}, active={}",
                to_join.size(), Pending(), ActiveTasks());
}

void ThreadPool::ShutDown(ShutDownOption opt, std::chrono::milliseconds timeout) {
    TP_PERF_SCOPE("ThreadPool::ShutDown");
    TP_LOG_INFO("ThreadPool shutdown requested: option={} timeout={}ms", opt, ToMilliseconds(timeout));
    if (opt == ShutDownOption::Graceful) {
        Stop(StopMode::Graceful);
        return;
    }

    if (opt == ShutDownOption::Force) {
        Stop(StopMode::Force);
        return;
    }

    auto deadline = std::chrono::steady_clock::now() + timeout;
    Stop(StopMode::Graceful);
    if (State() == PoolState::STOPPED) {
        TP_LOG_INFO("ThreadPool shutdown (Timeout) completed gracefully before deadline");
        return;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        TP_LOG_WARN("ThreadPool shutdown timeout exceeded; escalating to force stop");
        Stop(StopMode::Force);
    }
}

void ThreadPool::Post(std::function<void()> f) {
    // Use lightweight SimpleTask to avoid future overhead
    auto task_ptr = std::make_unique<SimpleTask>(std::move(f));
    
    // Fast state check
    for (;;) {
        PoolState s = state_.load(std::memory_order_acquire);
        if (s == PoolState::RUNNING) {
            break;
        }
        if (s == PoolState::PAUSED) {
            std::unique_lock<std::mutex> lk(pause_mtx_);
            paused_wait_cnt_.fetch_add(1, std::memory_order_relaxed);
            pause_cv_.wait(lk, [this] {
                auto t = state_.load(std::memory_order_acquire);
                return t != PoolState::PAUSED;
            });
            continue;
        }
        // Other states: reject the submission
        RecordTaskRejected();
        return;
    }

    // Dispatch by queue policy
    const auto policy = policy_.load(std::memory_order_relaxed);
    bool success = false;
    
    switch (policy) {
        case QueueFullPolicy::Block:
            success = queue_.WaitPush(std::move(task_ptr));
            break;
        case QueueFullPolicy::Discard:
            success = queue_.TryPush(std::move(task_ptr));
            if (!success) {
                discard_cnt_.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        case QueueFullPolicy::Overwrite: {
            TaskPtr overwritten;
            success = queue_.OverwritePush(std::move(task_ptr), &overwritten);
            if (overwritten) {
                overwritten->Cancel(std::make_exception_ptr(
                    std::runtime_error("ThreadPool::Post: overwritten")
                ));
                RecordTaskCancel();
                overwrite_cnt_.fetch_add(1, std::memory_order_relaxed);
            }
            if (!success) {
                discard_cnt_.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }
    }
    
    if (success) {
        total_submitted_.fetch_add(1, std::memory_order_relaxed);
    } else {
        RecordTaskRejected();
    }
}

void ThreadPool::Pause() noexcept {
    PoolState expected = PoolState::RUNNING;
    if (state_.compare_exchange_strong(expected, PoolState::PAUSED,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        TP_LOG_INFO("ThreadPool paused");
    } else {
        TP_LOG_DEBUG("ThreadPool pause ignored: state={}", expected);
    }
}

void ThreadPool::Resume() noexcept {
    PoolState expected = PoolState::PAUSED;
    if (state_.compare_exchange_strong(expected, PoolState::RUNNING,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lk(pause_mtx_);
        pause_cv_.notify_all();
        TP_LOG_INFO("ThreadPool resumed");
    } else {
        TP_LOG_DEBUG("ThreadPool resume ignored: state={}", expected);
    }
}

void ThreadPool::WorkerLoop(WorkerSlot* slot) {
    const auto tid_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    TP_LOG_DEBUG("Worker {} started (thread_id_hash={})",
                 static_cast<const void*>(slot), tid_hash);
    WorkerCounterHelper counter(*this, *slot);
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(pause_mtx_);
            while (state_.load(std::memory_order_acquire) == PoolState::PAUSED) {
                paused_wait_cnt_.fetch_add(1, std::memory_order_relaxed);
                TP_LOG_DEBUG("Worker {} waiting due to pool paused", static_cast<const void*>(slot));
                pause_cv_.wait(lk, [this] {
                    auto s = state_.load(std::memory_order_acquire);
                    return s != PoolState::PAUSED;
                });
            }
        }
        if (state_.load(std::memory_order_acquire) == PoolState::FORCE_STOPPING) {
            TP_LOG_DEBUG("Worker {} exiting because pool is force stopping", static_cast<const void*>(slot));
            break;
        }

    slot->idle.store(true, std::memory_order_release); // Mark thread idle
    slot->idle_nums.fetch_add(1, std::memory_order_relaxed); // Increment idle counter

        TaskPtr task;
        const bool ok = queue_.WaitPop(task);

        if (!ok) {
            if (queue_.Closed()) {
                TP_LOG_DEBUG("Worker {} exiting: task queue closed", static_cast<const void*>(slot));
                break;
            }
            TP_LOG_WARN("Worker {} wait-pop failed but queue open; retrying", static_cast<const void*>(slot));
            continue; // No task obtained; remain idle
        }

        if (!task) {
            TP_LOG_WARN("Worker {} woke without task object", static_cast<const void*>(slot));
            continue;
        }

        if (auto* exit_task = dynamic_cast<ExitTask*>(task.get())) {
            if (exit_task->slot == slot) {
                slot->should_exit.store(false, std::memory_order_release);
                TP_LOG_INFO("Worker {} received directed exit request", static_cast<const void*>(slot));
                break; // Directed exit
            }
            TP_LOG_DEBUG("Worker {} forwarding exit task to {}", static_cast<const void*>(slot),
                         static_cast<const void*>(exit_task->slot));
            if (!queue_.WaitPush(std::move(task))) {
                TP_LOG_WARN("Worker {} failed to requeue exit task", static_cast<const void*>(slot));
                break;
            }
            continue;
        }

        slot->last_active = std::chrono::steady_clock::now();
        counter.TaskOn();
        std::chrono::nanoseconds exec_span{0};
        bool exception_thrown = false;
        bool unknown_exception = false;
        std::string exception_message;
        {
            TP_PERF_SCOPE_HOOK_LEVEL(
                "WorkerLoop::ExecuteTask",
                ([this, &task, &exec_span](std::chrono::nanoseconds ns) {
                    exec_span = ns;
                    RecordTaskComplete(*task, ns);
                }),
                spdlog::level::trace);
            try {
                task->Execute();
            } catch (const std::exception& ex) {
                exception_thrown = true;
                exception_message = ex.what();
            } catch (...) {
                unknown_exception = true;
            }
        }
        counter.TaskOff();

        const auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(exec_span).count();
        if (exception_thrown) {
            TP_LOG_ERROR("Worker {} caught exception: {} (task={}, duration={}us)",
                         static_cast<const void*>(slot), exception_message,
                         static_cast<const void*>(task.get()), duration_us);
            continue;
        }
        if (unknown_exception) {
            TP_LOG_ERROR("Worker {} caught unknown exception (task={}, duration={}us)",
                         static_cast<const void*>(slot),
                         static_cast<const void*>(task.get()), duration_us);
            continue;
        }

        TP_LOG_DEBUG("Worker {} completed task={} success={} duration={}us pending={} active={}",
                     static_cast<const void*>(slot),
                     static_cast<const void*>(task.get()),
                     task->Success(),
                     duration_us,
                     Pending(), ActiveTasks());

        if (ActiveTasks() == 0 && Pending() == 0) {
            std::lock_guard<std::mutex> lk(drain_mtx_);
            drain_cv_.notify_all();
        }
    }
    TP_LOG_DEBUG("Worker {} exiting loop", static_cast<const void*>(slot));
}

bool ThreadPool::Running() const noexcept {
    return state_.load(std::memory_order_acquire) == PoolState::RUNNING;
}

bool ThreadPool::Paused() const noexcept {
    return state_.load(std::memory_order_acquire) == PoolState::PAUSED;
}

std::size_t ThreadPool::Pending() const noexcept {
    return queue_.Size();
}

std::size_t ThreadPool::ActiveTasks() const noexcept {
    return active_tasks_.load(std::memory_order_acquire);
}

PoolState ThreadPool::State() const noexcept {
    return state_.load(std::memory_order_acquire);
}

void ThreadPool::SetState(PoolState new_state) noexcept {
    state_.store(new_state, std::memory_order_release);
}

QueueFullPolicy ThreadPool::GetQueueFullPolicy() const noexcept {
    return policy_.load(std::memory_order_acquire);
}

void ThreadPool::SetQueueFullPolicy(QueueFullPolicy policy) noexcept {
    policy_.store(policy, std::memory_order_release);
}

void ThreadPool::SubmitOn() noexcept {
    submit_ing_.fetch_add(1, std::memory_order_acq_rel);
}

void ThreadPool::SubmitOff() noexcept {
    const auto prev = submit_ing_.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        std::lock_guard<std::mutex> lk(submit_mtx_);
        submit_cv_.notify_all();
    }
}

std::size_t ThreadPool::DiscardedTasks() const noexcept {
    return discard_cnt_.load(std::memory_order_relaxed);
}

std::size_t ThreadPool::OverwrittedTasks() const noexcept {
    return overwrite_cnt_.load(std::memory_order_relaxed);
}

std::size_t ThreadPool::PausedWait() const noexcept {
    return paused_wait_cnt_.load(std::memory_order_relaxed);
}

// Dynamic thread management utilities
void ThreadPool::LaunchLoadBalancer() {
    // Start the balancer thread
    TP_LOG_DEBUG("Launching load balancer thread (interval={}ms, cooldown={}ms)",
                 ToMilliseconds(load_check_interval_), ToMilliseconds(cooldown_));
    load_balancer_ = std::thread([this] { 
        LoadBalancerLoop(); 
    });
}

void ThreadPool::StopLoadBalancer() {
    balancer_stop_.store(true, std::memory_order_release);
    load_cv_.notify_all();
    TP_LOG_DEBUG("Stopping load balancer thread");
    // Join the balancer thread
    if (load_balancer_.joinable()) {
        load_balancer_.join();
        TP_LOG_DEBUG("Load balancer thread joined");
    }
}

void ThreadPool::LoadBalancerLoop() {
    std::unique_lock<std::mutex> lk(load_cv_mu_);
    auto last_adjust = std::chrono::steady_clock::now();
    std::size_t up_hits = 0;
    std::size_t down_hits = 0;

    while (!balancer_stop_.load(std::memory_order_acquire)) {
        load_cv_.wait_for(lk, load_check_interval_, [this] {
            return balancer_stop_.load(std::memory_order_acquire)
            || balancer_kick_.load(std::memory_order_acquire);
        });

        if (balancer_stop_.load(std::memory_order_acquire)) {
            break;        
        }
        const bool kicked = balancer_kick_.exchange(false, std::memory_order_acq_rel);
        const auto now = std::chrono::steady_clock::now();
        if (!kicked && now - last_adjust < cooldown_) {
            continue;
        }

        const std::size_t pending = queue_.Size();
        const std::size_t current = current_threads_.load(std::memory_order_acquire);
        const std::size_t active = active_threads_.load(std::memory_order_acquire);
        const double busy_ratio = current == 0 ? 0.0 : static_cast<double>(active) / current;

        busy_ratio_.store(busy_ratio, std::memory_order_release); // Update busy ratio
        pending_ratio_.store(static_cast<double>(pending) / queue_.Capacity(), std::memory_order_relaxed); // Update queue utilization

        // Scale-up conditions: too many pending tasks / workers too busy
        const bool to_grow = pending >= pending_hi_ || busy_ratio >= scale_up_threshold_;
        // Scale-down condition
        const bool to_shrink = pending <= pending_low_ && busy_ratio <= scale_down_threshold_;

        if (to_grow) {
            // Require multiple hits before scaling up (debounce)
            ++up_hits;
            if (up_hits >= debounce_hits_) {
                // Adjust capacity; reset hit counters
                up_hits = down_hits = 0;
                last_adjust = now;
                std::lock_guard<std::mutex> guard(workers_mu_);
                if (current_threads_.load(std::memory_order_acquire) < max_threads_) {
                    const auto before = current_threads_.load(std::memory_order_acquire);
                    CreateWorkerUnlocked();
                    TP_LOG_INFO("Load balancer scaled up: {} -> {} (pending={}, busy_ratio={:.2f})",
                                before, current_threads_.load(std::memory_order_acquire),
                                pending, busy_ratio);
                } else {
                    TP_LOG_DEBUG("Load balancer scale-up skipped: already at max_threads={}", max_threads_);
                }
            }
            continue;
        }

        if (to_shrink) {
            // Require multiple hits before scaling down (debounce)
            ++down_hits;
            if (down_hits >= debounce_hits_) {
                up_hits = down_hits = 0;
                last_adjust = now;
                std::vector<WorkerSlot*> target_workers;
                {
                    std::lock_guard<std::mutex> guard(workers_mu_);
                    if (current_threads_.load(std::memory_order_acquire) > core_threads_) {
                        target_workers = ScheduleShrinkUnlocked(1);
                    }
                }
                if (!target_workers.empty()) {
                    EnqueueExitSignals(target_workers);
                    for (WorkerSlot* worker : target_workers) {
                        if (worker) {
                            RetireWorkerUnlocked(*worker);
                        }
                    }
                    TP_LOG_INFO("Load balancer scaled down to {} workers (pending={}, busy_ratio={:.2f})",
                                current_threads_.load(std::memory_order_acquire),
                                pending, busy_ratio);
                } else {
                    TP_LOG_DEBUG("Load balancer scale-down skipped: no idle workers available");
                }
            }
            continue;
        }

        up_hits = down_hits = 0;
    }
    TP_LOG_DEBUG("Load balancer loop exiting");
}

void ThreadPool::CreateWorkerUnlocked() {
    auto slot = std::make_unique<WorkerSlot>();
    slot->last_active = std::chrono::steady_clock::now();
    WorkerSlot* raw = slot.get();

    slot->thread = std::thread([this, raw] {
        WorkerLoop(raw);
    });
    const auto thread_id = slot->thread.get_id();
    const auto thread_token = std::hash<std::thread::id>{}(thread_id);
    workers_.push_back(std::move(slot));
    current_threads_.fetch_add(1, std::memory_order_acq_rel);
    total_threads_created_.fetch_add(1, std::memory_order_relaxed); // Increment total created
    
    // Update peak thread count
    auto prev_peak = peak_threads_.load(std::memory_order_relaxed);
    auto cur_threads = current_threads_.load(std::memory_order_relaxed);
    while (cur_threads > prev_peak 
        && !peak_threads_.compare_exchange_weak(
            prev_peak, cur_threads
            , std::memory_order_release
            , std::memory_order_relaxed)) {} 
    TP_LOG_DEBUG("Worker {} created (thread_id_hash={}, current_threads={}, peak_threads={})",
                 static_cast<const void*>(raw), thread_token,
                 current_threads_.load(std::memory_order_relaxed),
                 peak_threads_.load(std::memory_order_relaxed));
}

std::vector<ThreadPool::WorkerSlot*> ThreadPool::ScheduleShrinkUnlocked(std::size_t count) {
    std::vector<WorkerSlot*> targets;
    targets.reserve(count);
    for (auto& slot : workers_) {
        if (targets.size() >= count) {
            break;
        }
        // Skip busy workers
        if (!slot->idle.load(std::memory_order_acquire)) {
            continue;
        }
        bool expected = false;
        // Mark worker for exit
        if (slot->should_exit.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            targets.push_back(slot.get());
        }
    }
    if (!targets.empty()) {
        TP_LOG_DEBUG("Scheduled {} worker(s) for shrink", targets.size());
    }
    return targets;
}

void ThreadPool::EnqueueExitSignals(const std::vector<WorkerSlot*>& targets) {
    for (WorkerSlot* slot : targets) {
        if (!slot) {
            continue;
        }
        // Create a directed exit task
        TaskPtr exit_task = std::make_unique<ExitTask>(slot);
        if (!queue_.WaitPush(std::move(exit_task))) {
            break;
        }
        TP_LOG_DEBUG("Exit signal enqueued for worker {}", static_cast<const void*>(slot));
    }
}

void ThreadPool::RetireWorkerUnlocked(WorkerSlot& slot) {
    std::unique_ptr<WorkerSlot> target_worker;
    {
        std::lock_guard<std::mutex> guard(workers_mu_);
        // Find the worker to retire
        auto it = std::find_if(workers_.begin(), workers_.end(),
            [&slot](const std::unique_ptr<WorkerSlot>& ptr) {
                return ptr.get() == &slot;
            });
        if (it == workers_.end()) {
            return;
        }
        target_worker = std::move(*it);
        // Erase worker from pool
        workers_.erase(it);
    }

    // Wait for worker thread to finish
    if (target_worker && target_worker->thread.joinable()) {
        const auto self = std::this_thread::get_id();
        if (target_worker->thread.get_id() != self) {
            target_worker->thread.join();
        }
    }
    TP_LOG_DEBUG("Worker {} retired; current_threads={}",
                 static_cast<const void*>(&slot),
                 current_threads_.load(std::memory_order_acquire));
}

void ThreadPool::TriggerLoadCheck() {
    balancer_kick_.store(true, std::memory_order_release);
    load_cv_.notify_one();
    TP_LOG_TRACE("Manual load check triggered");
}

std::size_t ThreadPool::CurrentThreads() const noexcept {
    return current_threads_.load(std::memory_order_acquire);
}

std::size_t ThreadPool::ActiveThreads() const noexcept {
    return active_threads_.load(std::memory_order_acquire);
}

ThreadPool::WorkerCounterHelper::WorkerCounterHelper(ThreadPool& pool, WorkerSlot& slot) noexcept
    : pool_(pool), slot_(slot) {
    slot_.idle.store(true, std::memory_order_release);
    slot_.idle_nums.store(0, std::memory_order_relaxed);
}

void ThreadPool::WorkerCounterHelper::TaskOn() {
    slot_.idle.store(false, std::memory_order_release); // Mark thread busy
    slot_.idle_nums.store(0, std::memory_order_relaxed); // Reset idle counter
    pool_.active_threads_.fetch_add(1, std::memory_order_acq_rel);
    pool_.active_tasks_.fetch_add(1, std::memory_order_acq_rel);
    normal_end_.store(true, std::memory_order_release);
}

void ThreadPool::WorkerCounterHelper::TaskOff() noexcept {
    if (!normal_end_.load(std::memory_order_acquire)) {
        return;
    }
    normal_end_.store(false, std::memory_order_release);
    pool_.active_tasks_.fetch_sub(1, std::memory_order_acq_rel);
    pool_.active_threads_.fetch_sub(1, std::memory_order_acq_rel);
    slot_.idle.store(true, std::memory_order_release);
    slot_.idle_nums.fetch_add(1, std::memory_order_relaxed);
}

ThreadPool::WorkerCounterHelper::~WorkerCounterHelper() noexcept {
    TaskOff();
    pool_.current_threads_.fetch_sub(1, std::memory_order_acq_rel);
    pool_.total_threads_destroyed_.fetch_add(1, std::memory_order_relaxed); // Increment total destroyed
    {
        std::lock_guard<std::mutex> lk(pool_.drain_mtx_);
        pool_.drain_cv_.notify_all();
    }
}

// Statistics
Statistics ThreadPool::GetStatistics() const noexcept {
    Statistics stats;
    // Overview
    stats.statistic_total_submitted = total_submitted_.load(std::memory_order_relaxed);
    stats.statistic_total_completed = total_completed_.load(std::memory_order_relaxed);
    stats.statistic_total_failed = total_failed_.load(std::memory_order_relaxed);
    stats.statistic_total_cancelled = total_cancelled_.load(std::memory_order_relaxed);
    stats.statistic_total_rejected = total_rejected_.load(std::memory_order_relaxed);
    // Execution time
    const auto exec_ns = total_exec_time_ns_.load(std::memory_order_acquire);
    stats.statistic_total_exec_time = std::chrono::nanoseconds(exec_ns);           
    stats.statistic_avg_exec_time = (stats.statistic_total_completed == 0) 
                                    ? std::chrono::nanoseconds{0} 
                                    : std::chrono::nanoseconds(exec_ns / stats.statistic_total_completed);
    // Load metrics
    const auto pending = Pending();
    stats.statistic_pending_tasks = pending;
    stats.statistic_busy_ratio = busy_ratio_.load(std::memory_order_relaxed);
    const auto cap = queue_.Capacity();
    stats.statistic_pending_ratio = cap == 0
        ? 0.0
        : static_cast<double>(pending) / static_cast<double>(cap);

    // Thread stats
    stats.statistic_current_threads = CurrentThreads();
    stats.statistic_active_threads = ActiveThreads(); 
    stats.statistic_peak_threads = peak_threads_.load(std::memory_order_relaxed);                                      
    stats.statistic_total_threads_created = total_threads_created_.load(std::memory_order_relaxed);
    stats.statistic_total_threads_destroyed = total_threads_destroyed_.load(std::memory_order_relaxed);

    // Policy-related
    stats.statistic_discard_cnt = DiscardedTasks();
    stats.statistic_overwrite_cnt = OverwrittedTasks();
    stats.statistic_paused_wait_cnt = PausedWait();
    return stats;
}

void ThreadPool::ResetStatistics() noexcept {
    total_submitted_.store(0, std::memory_order_relaxed); 
    total_completed_.store(0, std::memory_order_relaxed); 
    total_failed_.store(0, std::memory_order_relaxed); 
    total_cancelled_.store(0, std::memory_order_relaxed); 
    total_rejected_.store(0, std::memory_order_relaxed);

    total_exec_time_ns_.store(0, std::memory_order_relaxed);

    busy_ratio_.store(0.0, std::memory_order_relaxed);
    pending_ratio_.store(0.0, std::memory_order_relaxed);

    peak_threads_.store(CurrentThreads(), std::memory_order_relaxed);
    total_threads_created_.store(0, std::memory_order_relaxed);
    total_threads_destroyed_.store(0, std::memory_order_relaxed);
    
    discard_cnt_.store(0, std::memory_order_relaxed);
    overwrite_cnt_.store(0, std::memory_order_relaxed);
    paused_wait_cnt_.store(0, std::memory_order_relaxed);
}

void ThreadPool::RecordTaskComplete(const TaskBase& task, std::chrono::steady_clock::duration duration) noexcept {
    const auto elapsed_ns = static_cast<std::size_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
    total_exec_time_ns_.fetch_add(elapsed_ns, std::memory_order_relaxed);  // Accumulate total execution time
    if (task.Success()) {
        total_completed_.fetch_add(1, std::memory_order_relaxed);  // Success count +1
    } else {
        total_failed_.fetch_add(1, std::memory_order_relaxed);  // Failure count +1
    }
}

void ThreadPool::RecordTaskCancel() noexcept {
    const auto cancelled = total_cancelled_.fetch_add(1, std::memory_order_relaxed) + 1; // Cancel count +1
    try {
        TP_LOG_DEBUG("Task cancelled; total_cancelled={}", cancelled);
    } catch (...) {}
}

void ThreadPool::RecordTaskRejected() noexcept {
    const auto rejected = total_rejected_.fetch_add(1, std::memory_order_relaxed) + 1; // Rejected count +1
    try {
        TP_LOG_DEBUG("Task rejected; total_rejected={}, pending={}", rejected, Pending());
    } catch (...) {}
} 
} 
