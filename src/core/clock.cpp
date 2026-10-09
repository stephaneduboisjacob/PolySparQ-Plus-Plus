#include "clock.hpp"

namespace vp {

std::int64_t Clock::mono_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               mono_t::now().time_since_epoch())
        .count();
}

std::int64_t Clock::wall_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               wall_t::now().time_since_epoch())
        .count();
}

} // namespace vp

