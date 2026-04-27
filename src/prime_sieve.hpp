#pragma once
// Shared segmented prime sieve implementation.
// All prime calculator executables include this to avoid code duplication.

#include <cstdint>
#include <vector>
#include <cmath>
#include <cstring>

namespace prime {

// Segmented sieve of Eratosthenes: O(n log log n).
// Returns all primes in the half-open interval [start, end).
// Bit-packed odd-only sieve: 1 bit per odd number → 16x less memory.
// Word-level marking reduces read-modify-write operations.
// Thread-local caching minimizes allocation on hot paths.
inline std::vector<uint64_t> segmented_sieve(uint64_t start, uint64_t end) {
    if (end <= 2) [[unlikely]] return {};
    if (start < 2) start = 2;

    uint64_t limit = static_cast<uint64_t>(std::sqrt(static_cast<double>(end))) + 1;

    // Thread-local cache for small primes — only rebuilds when limit changes.
    thread_local uint64_t cached_limit = 0;
    thread_local std::vector<uint64_t> cached_small_primes;

    if (limit != cached_limit) [[unlikely]] {
        // Small sieve: bit-packed, odd-only
        size_t odd_count = (limit > 3) ? (limit - 3) / 2 + 1 : 0;
        std::vector<uint64_t> odd_sieve((odd_count + 63) / 64, ~0ULL);

        auto is_odd_prime = [&](uint64_t n) -> bool {
            if (n < 3) return false;
            size_t idx = (n - 3) / 2;
            return odd_sieve[idx / 64] & (1ULL << (idx % 64));
        };

        auto clear_odd = [&](uint64_t n) {
            size_t idx = (n - 3) / 2;
            odd_sieve[idx / 64] &= ~(1ULL << (idx % 64));
        };

        for (uint64_t i = 0; i * i <= limit; i++) {
            uint64_t p = (i == 0) ? 3 : 2 * i + 3;
            if (p * p > limit) break;
            if (is_odd_prime(p)) {
                for (uint64_t j = p * p; j <= limit; j += 2 * p) {
                    clear_odd(j);
                }
            }
        }

        cached_small_primes.clear();
        cached_small_primes.push_back(2);
        if (limit >= 3) {
            cached_small_primes.reserve(limit / 10);
            for (size_t i = 0; i < odd_count; i++) {
                if (odd_sieve[i / 64] & (1ULL << (i % 64))) {
                    cached_small_primes.push_back(3 + 2 * i);
                }
            }
        }

        cached_limit = limit;
    }

    // Segment sieve: bit-packed, odd-only
    // first_odd = first odd number >= start
    // bit index b represents odd number (first_odd + 2*b)
    uint64_t first_odd = start | 1;
    size_t odd_bits = (end > first_odd) ? (end - first_odd + 1) / 2 : 0;

    if (odd_bits == 0) [[unlikely]] {
        std::vector<uint64_t> result;
        if (start == 2) result.push_back(2);
        return result;
    }

    thread_local std::vector<uint64_t> seg;
    size_t seg_words = (odd_bits + 63) / 64;
    seg.assign(seg_words, ~0ULL);

    // Word-level marking: accumulate mask per word, apply once
    for (uint64_t p : cached_small_primes) {
        if (p == 2) continue;

        uint64_t first = ((start + p - 1) / p) * p;
        if (first == p) first += p;
        if (first >= end) continue;
        if ((first & 1) == 0) first += p;
        if (first >= end) continue;

        size_t bit = (first - first_odd) / 2;
        size_t stride = p;
        size_t cur_word = bit >> 6;
        uint64_t mask = 0;

        for (; bit < odd_bits; bit += stride) {
            size_t w = bit >> 6;
            if (w != cur_word) {
                seg[cur_word] &= ~mask;
                cur_word = w;
                mask = 0;
            }
            mask |= 1ULL << (bit & 63);
        }
        seg[cur_word] &= ~mask;
    }

    // Collect primes: scan words with ctz for fast bit extraction
    std::vector<uint64_t> result;
    result.reserve((end - start) / 15);

    if (start == 2) {
        result.push_back(2);
    }

    for (size_t w = 0; w < seg_words; w++) {
        uint64_t word = seg[w];
        if (word == 0) continue;
        size_t base_bit = w << 6;
        while (word) {
            size_t tz = __builtin_ctzll(word);
            size_t bit = base_bit + tz;
            if (bit >= odd_bits) break;
            result.push_back(first_odd + (bit << 1));
            word &= word - 1;
        }
    }

    return result;
}

} // namespace prime

namespace util {

// Fast uint64-to-string — no allocation, writes directly to buffer.
// Returns pointer past the last written character.
inline char* fast_uint64_to_str(uint64_t value, char* buffer) noexcept {
    if (value == 0) [[unlikely]] {
        *buffer++ = '0';
        return buffer;
    }
    char* end = buffer;
    while (value > 0) {
        *end++ = '0' + static_cast<char>(value % 10);
        value /= 10;
    }
    char* first = buffer;
    char* last = end - 1;
    while (first < last) {
        char tmp = *first;
        *first++ = *last;
        *last-- = tmp;
    }
    return end;
}

} // namespace util
