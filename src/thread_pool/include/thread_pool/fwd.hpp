#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <future>
#include <atomic>
#include <chrono>
#include <fmt/format.h>
#include <string_view>


namespace spdlog {
class logger;
}

namespace thread_pool {
enum class PoolState {
    CREATED = 0,
    RUNNING,
    SHUTTING_DOWN,  // No longer accepts new tasks; execute running/queued tasks; transition to STOPPED when all complete.
    FORCE_STOPPING, // No longer accepts new tasks; clear the task queue; exit quickly and transition to STOPPED.
    STOPPED,        // All threads have exited and resources are released
    PAUSED,         // Freeze task fetching while keeping worker threads
};

enum class StopMode {
    Graceful,
    Force,
};

enum class ShutDownOption {
    Graceful,
    Force,
    Timeout,
};

enum class QueueFullPolicy {
    Block,      // Block until space is available
    Discard,    // Drop the task
    Overwrite,  // Overwrite an existing (old) task
};

using LoggerPtr = std::shared_ptr<spdlog::logger>;

struct ThreadPoolConfig {
    std::size_t               queue_cap{1024};                       // Task queue capacity
    std::size_t               core_threads{4};                       // Core thread count
    std::size_t               max_threads{8};                        // Maximum thread count
    std::chrono::milliseconds load_check_interval{100};              // Load balancer sampling interval
    std::chrono::milliseconds keep_alive{5000};                      // Idle thread keep-alive time
    double                    scale_up_threshold{0.75};              // Busy ratio upper bound; scale up when exceeded
    double                    scale_down_threshold{0.25};            // Busy ratio lower bound; scale down when below
    std::size_t               pending_hi{64};                        // Pending (queue length) upper threshold
    std::size_t               pending_low{8};                        // Pending lower threshold
    std::size_t               debounce_hits{3};                      // Debounce hit count
    std::chrono::milliseconds cooldown{500};                         // Cooldown after capacity change
    QueueFullPolicy           queue_policy{QueueFullPolicy::Block};  // Backpressure policy
};

struct Statistics {
    std::size_t statistic_total_submitted{0};  // Total tasks successfully submitted
    std::size_t statistic_total_completed{0};  // Total tasks successfully executed
    std::size_t statistic_total_failed{0};     // Total tasks failed during execution
    std::size_t statistic_total_cancelled{0};  // Total tasks cancelled
    std::size_t statistic_total_rejected{0};   // Total tasks rejected on submit

    std::chrono::nanoseconds statistic_total_exec_time{0};  // Total execution time
    std::chrono::nanoseconds statistic_avg_exec_time{0};    // Average execution time

    std::size_t statistic_pending_tasks{0};    // Tasks currently pending in queue
    double      statistic_busy_ratio{0.0};     // Busy thread ratio
    double      statistic_pending_ratio{0.0};  // Queue utilization ratio

    std::size_t statistic_current_threads{0};          // Current live threads
    std::size_t statistic_active_threads{0};           // Threads currently executing tasks
    std::size_t statistic_peak_threads{0};             // Peak live threads
    std::size_t statistic_total_threads_created{0};    // Total threads created
    std::size_t statistic_total_threads_destroyed{0};  // Total threads destroyed

    std::size_t statistic_discard_cnt{0};    // Discarded task count
    std::size_t statistic_overwrite_cnt{0};  // Overwritten task count
    std::size_t statistic_paused_wait_cnt{0};     // Wait-for-task count
};
class TaskBase {
public:
    virtual ~TaskBase() = default;
    virtual void Execute() noexcept = 0;
    virtual bool Success() const noexcept {
        return true;
    }
    virtual void Cancel(std::exception_ptr eptr) noexcept = 0;
};

// Task with return value
template <typename T>
class FutureTask : public TaskBase  {
public:
    using Func = std::function<T()>;

    explicit FutureTask(Func f) : f_(std::move(f)) {}

    std::future<T> GetFuture() {
        if (fut_taken_.exchange(true, std::memory_order_acq_rel)) {
            throw std::runtime_error("Future already taken");
        }
        return promise_.get_future();
    }

    void Execute() noexcept override {
        if (done_.load(std::memory_order_acquire)) {
            return;
        }
        try {
            if constexpr (std::is_void_v<T>) {
                // Guard
                f_();
                promise_.set_value();
            } else {
                T result = f_();
                promise_.set_value(std::move(result));
            }
            ok_.store(true, std::memory_order_release);
        } catch (...) {
            try {
                promise_.set_exception(std::current_exception());
            } catch (...) {}
            ok_.store(false, std::memory_order_release);
        }
        done_.store(true, std::memory_order_release);
    }

    bool Success() const noexcept override {
        return ok_.load(std::memory_order_acquire);
    }

    void Cancel(std::exception_ptr eptr) noexcept override {
        if (!done_.exchange(true, std::memory_order_acq_rel)) {
            if (!eptr) {
                eptr = std::make_exception_ptr(std::runtime_error("task cancelled"));
            }
            try {
                promise_.set_exception(std::move(eptr));
            } catch (...) {}
            ok_.store(false, std::memory_order_release);
        }
    }
private:
    Func f_;
    std::promise<T> promise_;
    std::atomic<bool> ok_{false};
    std::atomic<bool> fut_taken_{false};
    std::atomic<bool> done_{false};
};

// Specialization for void (no return value)
template <>
class FutureTask<void> : public TaskBase  {
public:
    using Func = std::function<void()>;

    explicit FutureTask(Func f) : f_(std::move(f)) {}

    std::future<void> GetFuture() {
        if (fut_taken_.exchange(true, std::memory_order_acq_rel)) {
            throw std::runtime_error("Future already taken");
        }
        return promise_.get_future();
    }

    void Execute() noexcept override {
        if (done_.load(std::memory_order_acquire)) {
            return;
        }
        try {
            f_();
            promise_.set_value();
            ok_.store(true, std::memory_order_release);
        } catch (...) {
            try {
                promise_.set_exception(std::current_exception());
            } catch (...) {}
            ok_.store(false, std::memory_order_release);
        }
        done_.store(true, std::memory_order_release);
    }

    bool Success() const noexcept override {
        return ok_.load(std::memory_order_acquire);
    }

    void Cancel(std::exception_ptr eptr) noexcept override {
        if (!done_.exchange(true, std::memory_order_acq_rel)) {
            if (!eptr) {
                eptr = std::make_exception_ptr(std::runtime_error("task cancelled"));
            }
            try {
                promise_.set_exception(std::move(eptr));
            } catch (...) {}
            ok_.store(false, std::memory_order_release);
        }
    }
private:
    Func f_;
    std::promise<void> promise_;
    std::atomic<bool> ok_{false};
    std::atomic<bool> fut_taken_{false};
    std::atomic<bool> done_{false};
};

// Lightweight task (no future overhead; used by Post)
class SimpleTask : public TaskBase {
public:
    using Func = std::function<void()>;
    
    explicit SimpleTask(Func f) : f_(std::move(f)) {}

    void Execute() noexcept override {
        if (done_.load(std::memory_order_acquire)) {
            return;
        }
        try {
            f_();
            ok_.store(true, std::memory_order_release);
        } catch (...) {
            ok_.store(false, std::memory_order_release);
        }
        done_.store(true, std::memory_order_release);
    }

    bool Success() const noexcept override {
        return ok_.load(std::memory_order_acquire);
    }

    void Cancel(std::exception_ptr) noexcept override {
        if (!done_.exchange(true, std::memory_order_acq_rel)) {
            ok_.store(false, std::memory_order_release);
        }
    }

private:
    Func f_;
    std::atomic<bool> ok_{false};
    std::atomic<bool> done_{false};
};

using TaskPtr = std::unique_ptr<TaskBase>;

class ThreadPool;
}

namespace fmt {

// PoolState formatter
template <>
struct formatter<thread_pool::PoolState> : formatter<std::string_view> {
    auto format(thread_pool::PoolState s, format_context& ctx) const {
        using S = thread_pool::PoolState;
        std::string_view name;
        switch (s) {
            case S::CREATED:
                name = "CREATED"; 
                break;
            case S::RUNNING:        
                name = "RUNNING"; 
                break;
            case S::SHUTTING_DOWN:  
                name = "SHUTTING_DOWN"; 
                break;
            case S::FORCE_STOPPING: 
                name = "FORCE_STOPPING"; 
                break;
            case S::STOPPED:        
                name = "STOPPED"; 
                break;
            case S::PAUSED:         
                name = "PAUSED"; 
                break;
            default:                
                name = "UNKNOWN"; 
                break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

// QueueFullPolicy formatter
template <>
struct formatter<thread_pool::QueueFullPolicy> : formatter<std::string_view> {
    auto format(thread_pool::QueueFullPolicy p, format_context& ctx) const {
        using P = thread_pool::QueueFullPolicy;
        std::string_view name;
        switch (p) {
            case P::Block:     
                name = "Block"; 
                break;
            case P::Discard:   
                name = "Discard"; 
                break;
            case P::Overwrite: 
                name = "Overwrite"; 
                break;
            default:           
                name = "Unknown"; 
                break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

// StopMode formatter
template <>
struct formatter<thread_pool::StopMode> : formatter<std::string_view> {
    auto format(thread_pool::StopMode mode, format_context& ctx) const {
        using M = thread_pool::StopMode;
        std::string_view name = "Unknown";
        switch (mode) {
            case M::Graceful: 
                name = "Graceful"; 
                break;
            case M::Force:    
                name = "Force"; 
                break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

// ShutDownOption formatter
template <>
struct formatter<thread_pool::ShutDownOption> : formatter<std::string_view> {
    auto format(thread_pool::ShutDownOption opt, format_context& ctx) const {
        using O = thread_pool::ShutDownOption;
        std::string_view name = "Unknown";
        switch (opt) {
            case O::Graceful: 
                name = "Graceful"; 
                break;
            case O::Force:    
                name = "Force"; 
                break;
            case O::Timeout:  
                name = "Timeout"; 
                break;
        }
        return formatter<std::string_view>::format(name, ctx);
    }
};

}
