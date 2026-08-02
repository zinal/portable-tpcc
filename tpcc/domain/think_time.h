#pragma once

#include "rng.h"
#include "workload_config.h"

#include <cmath>
#include <cstdint>
#include <string>

namespace NTpcc {

// TPC-C §5.2.5.4: Tt = -log(r) * mean, truncated at 10 * mean.
// Benchbase mode returns the configured mean unchanged (constant think time).
inline int64_t SampleThinkTimeMs(
    NDetail::TFastRng& rng,
    int64_t meanMs,
    EThinkTimeDistribution distribution)
{
    if (meanMs <= 0) {
        return 0;
    }
    if (distribution == EThinkTimeDistribution::Benchbase) {
        return meanMs;
    }

    const double r = RandomUnitInterval(rng);
    double thinkMs = -std::log(r) * static_cast<double>(meanMs);
    const double maxMs = 10.0 * static_cast<double>(meanMs);
    if (thinkMs > maxMs) {
        thinkMs = maxMs;
    }
    return static_cast<int64_t>(thinkMs);
}

inline int64_t SampleThinkTimeMs(int64_t meanMs, EThinkTimeDistribution distribution) {
    return SampleThinkTimeMs(NDetail::ThreadLocalFastRng(), meanMs, distribution);
}

inline const char* ThinkTimeDistributionToString(EThinkTimeDistribution distribution) {
    switch (distribution) {
        case EThinkTimeDistribution::Benchbase:
            return "benchbase";
        case EThinkTimeDistribution::Exponential:
        default:
            return "exponential";
    }
}

// Parses run-config / CLI values. Accepts "constant" as an alias for benchbase.
inline bool ParseThinkTimeDistribution(const std::string& value, EThinkTimeDistribution& out) {
    if (value.empty() || value == "exponential") {
        out = EThinkTimeDistribution::Exponential;
        return true;
    }
    if (value == "benchbase" || value == "constant") {
        out = EThinkTimeDistribution::Benchbase;
        return true;
    }
    return false;
}

} // namespace NTpcc
