#pragma once
#include <atomic>
#include <cstdint>
#include <utility>

namespace vp {

// Single-writer, many-reader seqlock for POD-ish payloads.
// Writer increments seq before+after writing; readers retry on odd seq.
template <class T>
class SeqLock {
public:
    SeqLock() = default;

    // Writer: provide a callable that mutates the current value.
    template <class Fn>
    void write(Fn&& fn) noexcept {
        auto s = seq_.load(std::memory_order_relaxed) + 1; // make odd
        seq_.store(s, std::memory_order_release);
        std::forward<Fn>(fn)(value_);
        seq_.store(s + 1, std::memory_order_release);      // make even
    }

    // Reader: spin until we get a stable read.
    T read() const noexcept {
        T copy;
        for (;;) {
            auto s1 = seq_.load(std::memory_order_acquire);
            if (s1 & 1u) continue;
            copy = value_;
            auto s2 = seq_.load(std::memory_order_acquire);
            if (s1 == s2) return copy;
        }
    }

    std::uint64_t generation() const noexcept {
        return seq_.load(std::memory_order_acquire);
    }

private:
    alignas(64) std::atomic<std::uint64_t> seq_{0};
    T                                      value_{};
};

} // namespace vp

