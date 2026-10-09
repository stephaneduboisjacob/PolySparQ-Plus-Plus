#pragma once
// Lightweight RFC-4122 v4 UUID generator using mt19937_64.
// Thread-safe: uses thread_local PRNG. Suitable for order IDs, settlement IDs, etc.
#include <cstdint>
#include <iomanip>
#include <random>
#include <sstream>
#include <string>

namespace vp::core {

inline std::string make_uuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    auto a = rng(), b = rng();
    std::ostringstream o;
    o << std::hex << std::setfill('0')
      << std::setw(8) << ((a >> 32) & 0xffffffffULL) << '-'
      << std::setw(4) << ((a >> 16) & 0xffffULL)     << '-'
      << std::setw(4) << ((a)       & 0xffffULL)     << '-'
      << std::setw(4) << ((b >> 48) & 0xffffULL)     << '-'
      << std::setw(12)<< ( b        & 0xffffffffffffULL);
    return o.str();
}

} // namespace vp::core
