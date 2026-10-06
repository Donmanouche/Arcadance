#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <optional>
#include <type_traits>

namespace midipad {

// Wait-free queue for exactly one producer thread and one consumer thread.
// push() never blocks or allocates: it fails when the queue is full.
template <typename T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>);

public:
    bool push(const T& value) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head - tail_.load(std::memory_order_acquire) == Capacity)
            return false;
        buffer_[head & (Capacity - 1)] = value;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    std::optional<T> pop() {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return std::nullopt;
        T value = buffer_[tail & (Capacity - 1)];
        tail_.store(tail + 1, std::memory_order_release);
        return value;
    }

private:
    std::array<T, Capacity> buffer_{};
    alignas(64) std::atomic<std::size_t> head_{0};  // written by the producer
    alignas(64) std::atomic<std::size_t> tail_{0};  // written by the consumer
};

}  // namespace midipad
