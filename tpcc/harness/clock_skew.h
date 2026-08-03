#pragma once

#include <chrono>
#include <cstdint>
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

} // namespace NTpcc
