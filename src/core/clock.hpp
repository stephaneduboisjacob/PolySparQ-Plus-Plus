#pragma once
#include <chrono>
#include <cstdint>

namespace vp {

// Single source of monotonic time for the entire engine.
// Wall-clock is only used for display + Postgres writes.
struct Clock {
    using mono_t = std::chrono::steady_clock;
    using wall_t = std::chrono::system_clock;

    static std::int64_t mono_ns() noexcept;
    static std::int64_t wall_ns() noexcept;
    static std::int64_t mono_us() noexcept { return mono_ns() / 1000; }
    static std::int64_t mono_ms() noexcept { return mono_ns() / 1'000'000; }
};

} // namespace vp

