#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace NTpcc {

struct TClockCalibration {
    std::chrono::system_clock::time_point MeasuredAt{};
    int64_t OffsetMs = 0;
    int64_t UncertaintyMs = 0;
    int64_t RttMs = 0;
    std::string TimeSource;
};

// Returns true when calibration is within maxSkewMs (validation disabled when maxSkewMs <= 0).
bool IsClockSkewWithinBudget(const TClockCalibration& cal, int64_t maxSkewMs);

std::string FormatClockSkewViolation(const TClockCalibration& cal, int64_t maxSkewMs);

// Cristian's algorithm over sampleCount RTT samples. sampleServerEpochMs opens no
// connections itself — the caller supplies a lambda that queries DBMS time.
TClockCalibration MeasureClockCalibrationWithSampler(
    const std::function<int64_t()>& sampleServerEpochMs,
    const std::string& timeSource = {},
    int sampleCount = 5);

} // namespace NTpcc
