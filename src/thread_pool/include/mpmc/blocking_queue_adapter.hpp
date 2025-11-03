#pragma once

#include "mpmc/bounded_circular_queue.hpp"

#include <condition_variable>
#include <mutex>
#include <chrono>
#include <optional>
#include <functional>
#include <tuple>
#include <iterator>
#include <type_traits>

template <typename T>
class BlockingQueueAdapter {
public:
    using value_type = T;
    using size_type =  typename BoundedCircularQueue<T>::size_type;

    explicit BlockingQueueAdapter(size_type capacity) : queue_(capacity) {}

    // Non-blocking APIs
    bool TryPush(const T& item) {
        if (Closed()) {
            return false;
        }
        // Lock-free fast path: try enqueue directly
        if (queue_.TryPush(item)) {
            const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
            // Notify only when queue transitions from empty to non-empty
            if (prev == 0) {
                not_empty_.notify_one();
            }
            return true;
        }
        discard_counter_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    bool TryPush(T&& item) {
        if (Closed()) {
            return false;
        }
        // Lock-free fast path: use TryPushWith to only move item on success
        auto try_push_item = [&](void* slot) {
            ::new (slot) T(std::move(item));
        };
        if (queue_.TryPushWith(try_push_item)) {
            const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
            // Notify only when queue transitions from empty to non-empty
            if (prev == 0) {
                not_empty_.notify_one();
            }
            return true;
        }
        discard_counter_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    template <class... Args>
    bool TryEmplace(Args&&... args) {
        if (Closed()) {
            return false;
        }
        // Lock-free fast path: try enqueue directly
        if (queue_.TryEmplace(std::forward<Args>(args)...)) {
            const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
            // Notify only when queue transitions from empty to non-empty
            if (prev == 0) {
                not_empty_.notify_one();
            }
            return true;
        }
        discard_counter_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool TryPop(T& out) {
        // Lock-free fast path: try dequeue directly
        if (queue_.TryPop(out)) {
            const auto prev = pending_count_.fetch_sub(1, std::memory_order_release);
            // Notify only when queue transitions from full to non-full
            if (prev == Capacity()) {
                not_full_.notify_one();
            }
            return true;
        }
        return false;
    }

    // Blocking APIs
    bool WaitPush(const T& item) {
        if (Closed()) {
            return false;
        }
        
        // Fast path: attempt lock-free enqueue first
        if (queue_.TryPush(item)) {
            const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
            if (prev == 0) {
                not_empty_.notify_one();
            }
            return true;
        }
        
        // Slow path: need to wait, then acquire lock
        std::unique_lock<std::mutex> lk(push_mutex_);
        for (;;) {
            if (Closed()) {
                return false;
            }
            if (queue_.TryPush(item)) {
                break;
            }
            not_full_.wait(lk);
        }
        const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
        lk.unlock();
        if (prev == 0) {
            not_empty_.notify_one();
        }
        return true;
    }
    bool WaitPush(T&& item) {
        if (Closed()) {
            return false;
        }

        T value(std::move(item));
        auto try_push_value = [&]() {
            return queue_.TryPushWith([&](void* slot) {
                ::new (slot) T(std::move(value));
            });
        };

        if (try_push_value()) {
            const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
            if (prev == 0) {
                not_empty_.notify_one();
            }
            return true;
        }

        std::unique_lock<std::mutex> lk(push_mutex_);
        for (;;) {
            if (Closed()) {
                return false;
            }
            if (try_push_value()) {
                const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
                lk.unlock();
                if (prev == 0) {
                    not_empty_.notify_one();
                }
                return true;
            }
            not_full_.wait(lk);
        }
    }
    template <class... Args>
    bool WaitEmplace(Args&&... args) {
        if (Closed()) {
            return false;
        }

        std::tuple<std::decay_t<Args>...> stored(std::forward<Args>(args)...);
        auto try_emplace = [&]() {
            return queue_.TryPushWith([&](void* slot) {
                std::apply([&](auto&... vals) {
                    ::new (slot) T(std::move(vals)...);
                }, stored);
            });
        };

        if (try_emplace()) {
            const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
            if (prev == 0) {
                not_empty_.notify_one();
            }
            return true;
        }

        std::unique_lock<std::mutex> lk(push_mutex_);
        for (;;) {
            if (Closed()) {
                return false;
            }
            if (try_emplace()) {
                const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
                lk.unlock();
                if (prev == 0) {
                    not_empty_.notify_one();
                }
                return true;
            }
            not_full_.wait(lk);
        }
    }

    bool WaitPop(T& out) {
        // Fast path: attempt lock-free dequeue first
        if (queue_.TryPop(out)) {
            pending_count_.fetch_sub(1, std::memory_order_release);
            not_full_.notify_one();
            return true;
        }
        
        // Slow path: need to wait, then acquire lock
        std::unique_lock<std::mutex> lk(pop_mutex_);
        while (!queue_.TryPop(out)) {
            if (Closed()) {
                return false;
            }
            not_empty_.wait(lk);
        }
        pending_count_.fetch_sub(1, std::memory_order_release);
        lk.unlock();
        not_full_.notify_one();
        return true;
    }

    // Timeout variants
    template <typename Rep, typename Period>
    bool WaitPushFor(const T& item, const std::chrono::duration<Rep, Period>& timeout) {
        if (Closed()) {
            return false;
        }
        
        // Fast path: attempt lock-free enqueue first
        if (queue_.TryPush(item)) {
            const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
            if (prev == 0) {
                not_empty_.notify_one();
            }
            return true;
        }
        
        // Slow path: need to wait
        std::unique_lock<std::mutex> lk(push_mutex_);
        auto deadline = std::chrono::steady_clock::now() + timeout;

        for (;;) {
            if (Closed()) {
                return false;
            }
            if (queue_.TryPush(item)) {
                break;
            }
            if (not_full_.wait_until(lk, deadline) == std::cv_status::timeout) {
                discard_counter_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
        lk.unlock();
        if (prev == 0) {
            not_empty_.notify_one();
        }
        return true;
    }
    template <typename Rep, typename Period>
    bool WaitPushFor(T&& item, const std::chrono::duration<Rep, Period>& timeout) {
        if (Closed()) {
            return false;
        }

        T value(std::move(item));
        auto try_push_value = [&]() {
            return queue_.TryPushWith([&](void* slot) {
                ::new (slot) T(std::move(value));
            });
        };

        if (try_push_value()) {
            const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
            if (prev == 0) {
                not_empty_.notify_one();
            }
            return true;
        }

        std::unique_lock<std::mutex> lk(push_mutex_);
        auto deadline = std::chrono::steady_clock::now() + timeout;

        for (;;) {
            if (Closed()) {
                return false;
            }
            if (try_push_value()) {
                const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
                lk.unlock();
                if (prev == 0) {
                    not_empty_.notify_one();
                }
                return true;
            }
            if (not_full_.wait_until(lk, deadline) == std::cv_status::timeout) {
                discard_counter_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
    }

    template <typename Rep, typename Period>
    bool WaitPopFor(T& out, const std::chrono::duration<Rep, Period>& timeout) {
        // Fast path: attempt lock-free dequeue first
        if (queue_.TryPop(out)) {
            pending_count_.fetch_sub(1, std::memory_order_release);
            not_full_.notify_one();
            return true;
        }
        
        // Slow path: need to wait
        std::unique_lock<std::mutex> lk(pop_mutex_);
        auto deadline = std::chrono::steady_clock::now() + timeout;
        
        while (!queue_.TryPop(out)) {
            if (Closed()) {
                return false;
            }
            if (not_empty_.wait_until(lk, deadline) == std::cv_status::timeout) {
                return false;
            }
        }
        pending_count_.fetch_sub(1, std::memory_order_release);
        lk.unlock();
        not_full_.notify_one();
        return true;
    }

    // Overwrite an old task
    bool OverwritePush(T&& item, T* overwritten) {
        if (Closed()) {
            return false;
        }

        // Fast path: try enqueue directly first
        std::optional<T> hold(std::forward<T>(item));
        auto try_push_hold = [&]() {
            return queue_.TryPushWith([&](void* slot) {
                ::new (slot) T(std::move(*hold));
            });
        };
        if (try_push_hold()) {
            pending_count_.fetch_add(1, std::memory_order_release);
            not_empty_.notify_one();
            return true;
        }

        // Need overwrite: use dedicated lock
        std::unique_lock<std::mutex> lk(overwrite_mutex_);
        if (Closed()) {
            return false;
        }

        // Enqueue failed; try to overwrite an existing task
        std::optional<T> tmp;
        bool popped = queue_.TryPopConsume([&](T&& value) {
            tmp.emplace(std::move(value));
        });
        if (!popped) {
            return false; // Neither overwritten nor enqueued
        }
        pending_count_.fetch_sub(1, std::memory_order_release);
        if (overwritten) {
            *overwritten = std::move(*tmp);
        }
        const bool ok = try_push_hold();
        if (ok) {
            pending_count_.fetch_add(1, std::memory_order_release);
        }

        lk.unlock(); 
        not_empty_.notify_one();
        return ok;
    }


    // Utilities
    size_type DiscardCount() const noexcept {
        return discard_counter_.load(std::memory_order_relaxed);
    }
    void ResetDiscardCounter() noexcept {
        discard_counter_.store(0, std::memory_order_relaxed);
    }

    size_type Capacity() const noexcept {
        return queue_.Capacity();
    }

    // Close semantics
    void Close() noexcept {
        close_.store(true, std::memory_order_release);
        // Wake all waiting threads
        not_empty_.notify_all();
        not_full_.notify_all();
    }

    bool Closed() const noexcept {
        return close_.load(std::memory_order_acquire);
    }

    void Clear() noexcept {
        // Lock-free clearing
        T tmp;
        while (queue_.TryPop(tmp)) {}
        pending_count_.store(0, std::memory_order_release);
        not_full_.notify_all();
    }

    template <class Visitor>
    void Clear(Visitor&& visitor) noexcept {
        T tmp;
        while (queue_.TryPop(tmp)) {
            try {
                std::invoke(std::forward<Visitor>(visitor), tmp);
            } catch (...) {
                // log the exception
            }
        }
        pending_count_.store(0, std::memory_order_release);
        not_full_.notify_all();
    }

    // Number of pending items
    size_type Size() const noexcept {
        return pending_count_.load(std::memory_order_acquire);
    }
    
    // Batch non-blocking enqueue
    template <typename Iterator>
    size_type TryPushBatch(Iterator begin, Iterator end) {
        if (Closed()) {
            return 0;
        }
        
        const size_type count = queue_.TryPushBatch(begin, end);
        if (count > 0) {
            const auto prev = pending_count_.fetch_add(count, std::memory_order_release);
            // Batch: notify only when the first element is enqueued
            if (prev == 0) {
                not_empty_.notify_all();  // For batch, use notify_all to wake multiple consumers
            }
        }
        return count;
    }

    // Batch non-blocking dequeue
    template <typename OutputIterator>
    size_type TryPopBatch(OutputIterator out, size_type max_count) {
        const size_type count = queue_.TryPopBatch(out, max_count);
        if (count > 0) {
            const auto prev = pending_count_.fetch_sub(count, std::memory_order_release);
            // Batch: notify when releasing space
            if (prev >= Capacity() - count && prev <= Capacity()) {
                not_full_.notify_all();  // For batch, use notify_all to wake multiple producers
            }
        }
        return count;
    }

    // Batch blocking enqueue
    template <typename Iterator>
    size_type WaitPushBatch(Iterator begin, Iterator end, size_type& total_count) {
        if (Closed()) {
            return 0;
        }

        total_count = static_cast<size_type>(std::distance(begin, end));
        if (total_count == 0) {
            return 0;
        }

        size_type pushed = TryPushBatch(begin, end);
        auto it = begin;
        std::advance(it, pushed);

        for (; it != end; ++it) {
            auto try_push_elem = [&]() {
                return queue_.TryPushWith([&](void* slot) {
                    ::new (slot) T(std::move(*it));
                });
            };

            if (try_push_elem()) {
                const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
                if (prev == 0) {
                    not_empty_.notify_one();
                }
                ++pushed;
                continue;
            }

            std::unique_lock<std::mutex> lk(push_mutex_);
            for (;;) {
                if (Closed()) {
                    return pushed;
                }
                if (try_push_elem()) {
                    const auto prev = pending_count_.fetch_add(1, std::memory_order_release);
                    lk.unlock();
                    if (prev == 0) {
                        not_empty_.notify_one();
                    }
                    ++pushed;
                    break;
                }
                not_full_.wait(lk);
            }
        }

        return pushed;
    }

    // Batch blocking dequeue
    template <typename OutputIterator>
    size_type WaitPopBatch(OutputIterator out, size_type max_count, size_type min_count = 1) {
        if (min_count == 0 || min_count > max_count) {
            min_count = 1;
        }

        size_type popped = TryPopBatch(out, max_count);
        if (popped >= min_count || popped == max_count) {
            return popped;
        }

        while (popped < min_count) {
            T item;
            if (!WaitPop(item)) {
                return popped;
            }
            *out++ = std::move(item);
            ++popped;
        }

        popped += TryPopBatch(out, max_count - popped);
        return popped;
    }

    // Batch consume with callback
    template <typename Func>
    size_type TryConsumeBatch(Func&& func, size_type max_count) {
        const size_type count = queue_.TryConsumeBatch(std::forward<Func>(func), max_count);
        if (count > 0) {
            const auto prev = pending_count_.fetch_sub(count, std::memory_order_release);
            if (prev >= Capacity() - count && prev <= Capacity()) {
                not_full_.notify_all();
            }
        }
        return count;
    }

private:
    // Optimization: use multiple fine-grained mutexes to reduce contention
    mutable std::mutex push_mutex_;       // Used only for waiting on push
    mutable std::mutex pop_mutex_;        // Used only for waiting on pop
    mutable std::mutex overwrite_mutex_;  // Used only for overwrite operation
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    BoundedCircularQueue<T> queue_;       // Lock-free queue
    std::atomic<size_type> discard_counter_{0};
    std::atomic<size_type> pending_count_{0};
    std::atomic<bool> close_{false};
};
