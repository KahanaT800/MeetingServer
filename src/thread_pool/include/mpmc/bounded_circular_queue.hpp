#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <vector>
#include <utility>
#include <new>
#include <limits>
#include <memory>
#include <type_traits>
#include <stdexcept>

template <typename T>
class BoundedCircularQueue {
public:
    using value_type = T;
    using size_type = std::size_t;

    // Construction/Destruction
    // Public constructor
    explicit BoundedCircularQueue(size_type capacity)
        : BoundedCircularQueue(AdjustedTag{}, AdjustCapacity(capacity)) {}
    ~BoundedCircularQueue() = default;

    // Disable copy/move
    BoundedCircularQueue(const BoundedCircularQueue&) = delete;
    BoundedCircularQueue& operator=(const BoundedCircularQueue&) = delete;
    BoundedCircularQueue(BoundedCircularQueue&&) = delete;
    BoundedCircularQueue& operator=(BoundedCircularQueue&&) = delete;

    // Core operations: enqueue/dequeue
    bool TryPush(const T& item) {
        return DoPush([&](void* p){
            ::new (p) T(item);
        });
    }
    bool TryPush(T&& item) {
        return DoPush([&](void* p){
            ::new (p) T(std::move(item));
        });
    }
    template <typename Producer>
    bool TryPushWith(Producer&& producer) {
        return DoPush(std::forward<Producer>(producer));
    }
    template <typename... Args>
    bool TryEmplace(Args&&... args) {
         return DoPush([&](void* p){
            ::new (p) T(std::forward<Args>(args)...);
        });
    }
    
    bool TryPop(T& out) {
        size_type pos = consumer_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = buffer_[pos & mask_];
            size_type seq = cell.seq_.load(std::memory_order_acquire);

            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                // Cell is consumable; try to claim it
                if (consumer_pos_.compare_exchange_weak(
                        pos, pos + 1
                        , std::memory_order_relaxed
                        , std::memory_order_relaxed
                )) {
                    // Claimed successfully
                    void* storage = cell.storage_;
                    T* elem = std::launder(reinterpret_cast<T*>(storage));
                    out = std::move(*elem);
                    elem->~T();

                    // Cleanup done; ready for next write round
                    cell.seq_.store(pos + capacity_, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                // Queue empty; the cell for this round has not been written; wait for producer
                return false;
            } else {
                // Another consumer claimed the cell; reload consumer_pos_ and retry
                pos = consumer_pos_.load(std::memory_order_relaxed);
            }
        }

    }

    template <class C>
    bool TryPopConsume(C&& out) {
        size_type pos = consumer_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = buffer_[pos & mask_];
            size_type seq = cell.seq_.load(std::memory_order_acquire);

            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                // Cell is consumable; try to claim it
                if (consumer_pos_.compare_exchange_weak(
                        pos, pos + 1
                        , std::memory_order_relaxed
                        , std::memory_order_relaxed
                )) {
                    // Claimed successfully
                    void* storage = cell.storage_;
                    T* elem = std::launder(reinterpret_cast<T*>(storage));
                    out(std::move(*elem)); // move as rvalue
                    elem->~T();

                     // Cleanup done; ready for next write round
                    cell.seq_.store(pos + capacity_, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                // Queue empty; the cell for this round has not been written; wait for producer
                return false;
            } else {
                // Another consumer claimed the cell; reload consumer_pos_ and retry
                pos = consumer_pos_.load(std::memory_order_relaxed);
            }
        }
    }

    // Observation utilities; approximate under concurrency
    size_type ApproxSize() const noexcept {
        const auto p = producer_pos_.load(std::memory_order_relaxed);
        const auto c = consumer_pos_.load(std::memory_order_relaxed);
        return static_cast<size_type>(p - c);
    }
    size_type Capacity() const noexcept {
        return capacity_;
    }
    bool Empty() const noexcept {
        return ApproxSize() == 0;
    }
    bool Full()  const noexcept {
        return ApproxSize() >= Capacity();
    }

    bool TryFront(T& out) const {
        size_type pos = consumer_pos_.load(std::memory_order_relaxed);
        Cell& cell = buffer_[pos & mask_];
        size_type seq = cell.seq_.load(std::memory_order_acquire);

        const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
        if (diff == 0) {
            // Cell is consumable
            void* storage = cell.storage_;
            T* elem = std::launder(reinterpret_cast<T*>(storage));
            out = *elem; // Observe only; no move
            return true;
        } else {
            // Queue empty; the cell for this round has not been written; wait for producer
            return false;
        }
    }

     // Batch enqueue (move semantics)
    template <typename Iterator>
    size_type TryPushBatch(Iterator begin, Iterator end) {
        size_type count = 0;
        for (auto it = begin; it != end; ++it) {
            if (!TryPushWith([&](void* p) {
                    ::new (p) T(std::move(*it));
                })) {
                break;
            }
            ++count;
        }
        return count;
    }

    // Batch dequeue
    template <typename OutputIterator>
    size_type TryPopBatch(OutputIterator out, size_type max_count) {
        size_type count = 0;
        T item;
        for (size_type i = 0; i < max_count; ++i) {
            if (!TryPop(item)) {
                break;
            }
            *out++ = std::move(item);
            ++count;
        }
        return count;
    }

    // Batch consume (with callback)
    template <typename Func>
    size_type TryConsumeBatch(Func&& func, size_type max_count) {
        size_type count = 0;
        for (size_type i = 0; i < max_count; ++i) {
            bool consumed = TryPopConsume([&](T&& item) {
                func(std::move(item));
            });
            if (!consumed) {
                break;
            }
            ++count;
        }
        return count;
    }

private:
    struct AdjustedTag { explicit AdjustedTag() = default; };

    // Constructor
    BoundedCircularQueue(AdjustedTag, size_type adjusted_capacity)
        : capacity_(adjusted_capacity)
        , mask_(capacity_ - 1)
        , buffer_(capacity_)
    {
        // Initialize sequence number for each slot
        for (size_type i = 0; i < capacity_; ++i) {
            buffer_[i].seq_.store(i, std::memory_order_relaxed);
        }
    }
    
    // Single slot (Cell)
    struct alignas(64) Cell {
        std::atomic<size_type> seq_{0};
        alignas(alignof(T)) unsigned char storage_[sizeof(T)];
        Cell() noexcept : storage_{} {}
    };

    static size_type RoundUpToPow2(size_type n) {
        if (n < 2) {return 2;}
        n--;
        for (size_type i = 1; i < sizeof(size_type) * 8; i <<= 1) {
            n |= (n >> i);
        }
        n++;
        return n;
    }

    static constexpr size_type AdjustCapacity(size_type n) noexcept {
        return RoundUpToPow2(n);
    }

    // Enqueue helper: claim cell storage and construct in-place
    template <typename Func>
    bool DoPush(Func&& f) {
        size_type pos = producer_pos_.load(std::memory_order_relaxed);
        for (;;) {
            Cell& cell = buffer_[pos & mask_];
            size_type seq = cell.seq_.load(std::memory_order_acquire);

            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                // Cell is writable; try to claim it
                if (producer_pos_.compare_exchange_weak(
                        pos, pos + 1
                        , std::memory_order_relaxed
                        , std::memory_order_relaxed
                )) {
                    // Claimed successfully
                    void* storage = cell.storage_;
                    try {
                        f(storage);  
                    } catch (...) {
                        // Construction failed; roll back the ticket
                        cell.seq_.store(pos, std::memory_order_release);
                        throw;
                    }
                    // Mark consumable
                    cell.seq_.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                // Queue full; this round's cell already used; wait for consumer
                return false;
            } else {
                // Another producer claimed the cell; reload producer_pos_ and retry
                pos = producer_pos_.load(std::memory_order_relaxed);
            }
        }
    }

private:
    const size_type capacity_;
    const size_type mask_; // equals capacity_ - 1
    std::vector<Cell> buffer_;

    alignas(64) std::atomic<size_type> producer_pos_{0};
    alignas(64) std::atomic<size_type> consumer_pos_{0};
};
