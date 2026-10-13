#pragma once
#include <array>
#include <atomic>
#include <cstdint>

namespace bvr::frame_pacing {
// One producer and one consumer. A full ring refuses the new sample immediately;
// diagnostics may lose samples, but must never hold up the rendering thread.
template<class T, uint32_t Capacity> class SampleQueue {
    static_assert(Capacity > 1 && Capacity < 0x80000000u, "bounded queue capacity");
    std::array<T, Capacity> data_{};
    std::atomic<uint32_t> write_{0};
    std::atomic<uint32_t> read_{0};
public:
    bool push(const T& value) {
        const uint32_t w = write_.load(std::memory_order_relaxed);
        if (w - read_.load(std::memory_order_acquire) >= Capacity) return false;
        data_[w % Capacity] = value;
        write_.store(w + 1, std::memory_order_release);
        return true;
    }
    bool pop(T& value) {
        const uint32_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire)) return false;
        value = data_[r % Capacity];
        read_.store(r + 1, std::memory_order_release);
        return true;
    }
};
} // namespace bvr::frame_pacing
