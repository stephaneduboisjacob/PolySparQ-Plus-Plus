#pragma once
// Compact Keccak-256 — Ethereum variant (padding byte 0x01, NOT SHA3 0x06).
// Single header, no external deps, standard C++17.
// Reference: https://keccak.team/specifications.html
//
// Usage:
//   std::array<uint8_t,32> hash = keccak256(data_ptr, data_len);
//
// Thread-safe (all state is on the stack).

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace keccak {

namespace detail {

static constexpr uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL,
};

static constexpr int RHO[25] = {
     0,  1, 62, 28, 27,
    36, 44,  6, 55, 20,
     3, 10, 43, 25, 39,
    41, 45, 15, 21,  8,
    18,  2, 61, 56, 14,
};

static constexpr int PI[25] = {
    0, 10, 20,  5, 15,
    16,  1, 11, 21,  6,
     7, 17,  2, 12, 22,
    23,  8, 18,  3, 13,
    14, 24,  9, 19,  4,
};

inline uint64_t rotl64(uint64_t x, int n) noexcept {
    return (x << n) | (x >> (64 - n));
}

inline void keccakf(uint64_t st[25]) noexcept {
    for (int round = 0; round < 24; ++round) {
        // Theta
        uint64_t bc[5];
        for (int x = 0; x < 5; ++x)
            bc[x] = st[x] ^ st[x+5] ^ st[x+10] ^ st[x+15] ^ st[x+20];
        for (int x = 0; x < 5; ++x) {
            uint64_t t = bc[(x+4)%5] ^ rotl64(bc[(x+1)%5], 1);
            for (int y = 0; y < 25; y += 5) st[y+x] ^= t;
        }
        // Rho + Pi
        uint64_t tmp[25];
        for (int i = 0; i < 25; ++i) tmp[PI[i]] = rotl64(st[i], RHO[i]);
        // Chi
        for (int y = 0; y < 25; y += 5) {
            uint64_t a0 = tmp[y], a1 = tmp[y+1], a2 = tmp[y+2],
                     a3 = tmp[y+3], a4 = tmp[y+4];
            st[y  ] = a0 ^ (~a1 & a2);
            st[y+1] = a1 ^ (~a2 & a3);
            st[y+2] = a2 ^ (~a3 & a4);
            st[y+3] = a3 ^ (~a4 & a0);
            st[y+4] = a4 ^ (~a0 & a1);
        }
        // Iota
        st[0] ^= RC[round];
    }
}

} // namespace detail

// Rate for Keccak-256: 1088 bits = 136 bytes
static constexpr size_t RATE = 136;

inline std::array<uint8_t, 32> hash(const void* data, size_t len) noexcept {
    uint64_t st[25] = {};
    const auto* p = static_cast<const uint8_t*>(data);
    size_t remaining = len;

    // Absorb full rate-sized blocks
    while (remaining >= RATE) {
        for (size_t i = 0; i < RATE / 8; ++i) {
            uint64_t w;
            memcpy(&w, p + i*8, 8);
            st[i] ^= w;
        }
        detail::keccakf(st);
        p         += RATE;
        remaining -= RATE;
    }

    // Last partial block with Ethereum padding (0x01, not SHA3's 0x06)
    uint8_t block[RATE] = {};
    memcpy(block, p, remaining);
    block[remaining]    = 0x01;   // Ethereum Keccak padding
    block[RATE - 1]    |= 0x80;
    for (size_t i = 0; i < RATE / 8; ++i) {
        uint64_t w;
        memcpy(&w, block + i*8, 8);
        st[i] ^= w;
    }
    detail::keccakf(st);

    // Squeeze 32 bytes
    std::array<uint8_t, 32> out;
    memcpy(out.data(), st, 32);
    return out;
}

// Convenience overloads
inline std::array<uint8_t, 32> hash(const std::array<uint8_t,32>& a) noexcept {
    return hash(a.data(), 32);
}

} // namespace keccak
