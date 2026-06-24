#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <optional>

namespace hft {

// Single-producer, single-consumer lock-free ring buffer.
// Cache-line separated head/tail to eliminate false sharing.
template <typename T, std::size_t Capacity>
class SPSCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire))
            return false; // full
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt; // empty
        T item = buffer_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return item;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    static constexpr std::size_t mask_ = Capacity - 1;
    static constexpr std::size_t cache_line_ = 64;

    alignas(cache_line_) std::atomic<std::size_t> head_{0};
    alignas(cache_line_) std::atomic<std::size_t> tail_{0};
    std::array<T, Capacity> buffer_{};
};

} // namespace hft
