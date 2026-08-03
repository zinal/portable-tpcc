#include "clock_skew.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace NTpcc {

namespace {

using SysClock = std::chrono::system_clock;

int64_t AbsInt64(int64_t v) {
    return v < 0 ? -v : v;
}

int64_t ToEpochMs(SysClock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

struct TSample {
    int64_t OffsetMs = 0;
    int64_t RttMs = 0;
};

} // anonymous

bool IsClockSkewWithinBudget(const TClockCalibration& cal, int64_t maxSkewMs) {
    if (maxSkewMs <= 0) {
        return true;
    }
    return AbsInt64(cal.OffsetMs) <= maxSkewMs && cal.UncertaintyMs <= maxSkewMs;
}

std::string FormatClockSkewViolation(const TClockCalibration& cal, int64_t maxSkewMs) {
    std::ostringstream ss;
    ss << "clock skew exceeds budget: offset_ms=" << cal.OffsetMs
       << " uncertainty_ms=" << cal.UncertaintyMs
       << " max_clock_skew_ms=" << maxSkewMs;
    if (!cal.TimeSource.empty()) {
        ss << " time_source=" << cal.TimeSource;
    }
    return ss.str();
}

TClockCalibration MeasureClockCalibrationWithSampler(
    const std::function<int64_t()>& sampleServerEpochMs,
    const std::string& timeSource,
    int sampleCount)
{
    if (!sampleServerEpochMs) {
        throw std::runtime_error("MeasureClockCalibrationWithSampler: sampler is empty");
    }
    if (sampleCount <= 0) {
        throw std::runtime_error("MeasureClockCalibrationWithSampler: sampleCount must be > 0");
    }

    TClockCalibration result;
    result.MeasuredAt = SysClock::now();
    result.TimeSource = timeSource;

    std::vector<TSample> samples;
    samples.reserve(static_cast<size_t>(sampleCount));
    for (int i = 0; i < sampleCount; ++i) {
        const auto t0 = SysClock::now();
        const int64_t serverMs = sampleServerEpochMs();
        const auto t1 = SysClock::now();
        const auto rttMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        const int64_t midpointMs = ToEpochMs(t0) + rttMs / 2;
        samples.push_back(TSample{serverMs - midpointMs, rttMs});
    }

    const auto best = *std::min_element(
        samples.begin(), samples.end(),
        [](const TSample& a, const TSample& b) { return a.RttMs < b.RttMs; });

    result.OffsetMs = best.OffsetMs;
    result.RttMs = best.RttMs;
    result.UncertaintyMs = (best.RttMs + 1) / 2;
    result.MeasuredAt = SysClock::now();
    return result;
}

} // namespace NTpcc
