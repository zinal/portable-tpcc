#pragma once

#include "constants.h"

#include <cstdint>
#include <random>
#include <string>

namespace NTpcc {

namespace NDetail {

// xorshift64* - tiny, fast, non-cryptographic PRNG. 64-bit state, 64-bit output.
// Period 2^64 - 1; ample for TPC-C data generation.
class TFastRng {
public:
    TFastRng() {
        std::random_device rd;
        uint64_t s = (static_cast<uint64_t>(rd()) << 32) | static_cast<uint64_t>(rd());
        State_ = s ? s : 0x9E3779B97F4A7C15ULL;
    }

    explicit TFastRng(uint64_t seed) {
        State_ = seed ? seed : 0x9E3779B97F4A7C15ULL;
    }

    uint64_t Next() {
        uint64_t x = State_;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        State_ = x;
        return x * 0x2545F4914F6CDD1DULL;
    }

    uint64_t State() const {
        return State_;
    }

private:
    uint64_t State_;
};

inline TFastRng& ThreadLocalFastRng() {
    thread_local TFastRng rng;
    return rng;
}

// Lemire's nearly-divisionless unbiased bounded random in [0, range).
// https://arxiv.org/abs/1805.10941
inline uint64_t BoundedRandom(TFastRng& rng, uint64_t range) {
    if (range == 0) {
        return 0;
    }
    uint64_t x = rng.Next();
    __uint128_t m = static_cast<__uint128_t>(x) * static_cast<__uint128_t>(range);
    uint64_t l = static_cast<uint64_t>(m);
    if (l < range) {
        uint64_t t = (0ULL - range) % range;
        while (l < t) {
            x = rng.Next();
            m = static_cast<__uint128_t>(x) * static_cast<__uint128_t>(range);
            l = static_cast<uint64_t>(m);
        }
    }
    return static_cast<uint64_t>(m >> 64);
}

inline uint64_t BoundedRandom(uint64_t range) {
    return BoundedRandom(ThreadLocalFastRng(), range);
}

} // namespace NDetail

// Explicitly seeded RNG for deterministic load and transaction inputs.
class TSeededRng {
public:
    explicit TSeededRng(uint64_t seed)
        : Rng_(seed)
    {}

    uint64_t Next() {
        return Rng_.Next();
    }

    // Derive an independent sub-stream seed (e.g. per warehouse / table).
    uint64_t Mix(uint64_t salt) const {
        uint64_t x = Rng_.State() ^ (salt + 0x9E3779B97F4A7C15ULL);
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return x ? x : 0xA0761D6478BD642FULL;
    }

    TSeededRng Fork(uint64_t salt) const {
        return TSeededRng(Mix(salt));
    }

    NDetail::TFastRng& Impl() {
        return Rng_;
    }

private:
    NDetail::TFastRng Rng_;
};

// [from; to] inclusive range
inline size_t RandomNumber(NDetail::TFastRng& rng, size_t from, size_t to) {
    return from + static_cast<size_t>(NDetail::BoundedRandom(rng, to - from + 1));
}

inline size_t RandomNumber(TSeededRng& rng, size_t from, size_t to) {
    return RandomNumber(rng.Impl(), from, to);
}

// Legacy thread-local RNG (non-deterministic). Prefer TSeededRng for load/txn inputs.
inline size_t RandomNumber(size_t from, size_t to) {
    return RandomNumber(NDetail::ThreadLocalFastRng(), from, to);
}

// Uniform open-closed unit interval (0, 1]. Avoids log(0) for exponential sampling.
inline double RandomUnitInterval(NDetail::TFastRng& rng) {
    // Map uint64 → (0, 1]: (x + 1) / 2^64.
    return (static_cast<double>(rng.Next()) + 1.0) * 0x1p-64;
}

inline double RandomUnitInterval() {
    return RandomUnitInterval(NDetail::ThreadLocalFastRng());
}

inline int NonUniformRandom(NDetail::TFastRng& rng, int A, int C, int min, int max) {
    int randomNum = static_cast<int>(RandomNumber(rng, 0, A));
    int randomNum2 = static_cast<int>(RandomNumber(rng, min, max));
    return (((randomNum | randomNum2) + C) % (max - min + 1)) + min;
}

inline int NonUniformRandom(TSeededRng& rng, int A, int C, int min, int max) {
    return NonUniformRandom(rng.Impl(), A, C, min, max);
}

inline int NonUniformRandom(int A, int C, int min, int max) {
    return NonUniformRandom(NDetail::ThreadLocalFastRng(), A, C, min, max);
}

inline int GetRandomCustomerID(TSeededRng& rng) {
    return NonUniformRandom(rng, 1023, C_ID_C, 1, CUSTOMERS_PER_DISTRICT);
}

inline int GetRandomCustomerID() {
    return NonUniformRandom(1023, C_ID_C, 1, CUSTOMERS_PER_DISTRICT);
}

inline int GetRandomItemID(TSeededRng& rng) {
    return NonUniformRandom(rng, 8191, OL_I_ID_C, 1, ITEM_COUNT);
}

inline int GetRandomItemID() {
    return NonUniformRandom(8191, OL_I_ID_C, 1, ITEM_COUNT);
}

constexpr const char* const NameTokens[] = {"BAR", "OUGHT", "ABLE", "PRI",
        "PRES", "ESE", "ANTI", "CALLY", "ATION", "EING"};

inline std::string GetLastName(int num) {
    std::string result;
    result += NameTokens[num / 100];
    result += NameTokens[(num / 10) % 10];
    result += NameTokens[num % 10];
    return result;
}

inline std::string GetNonUniformRandomLastNameForRun(TSeededRng& rng) {
    return GetLastName(NonUniformRandom(rng, 255, C_LAST_RUN_C, 0, 999));
}

inline std::string GetNonUniformRandomLastNameForRun() {
    return GetLastName(NonUniformRandom(255, C_LAST_RUN_C, 0, 999));
}

inline std::string GetNonUniformRandomLastNameForLoad(TSeededRng& rng) {
    return GetLastName(NonUniformRandom(rng, 255, C_LAST_LOAD_C, 0, 999));
}

inline std::string GetNonUniformRandomLastNameForLoad() {
    return GetLastName(NonUniformRandom(255, C_LAST_LOAD_C, 0, 999));
}

} // namespace NTpcc
