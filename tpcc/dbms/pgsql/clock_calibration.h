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
};

// Estimate local clock offset vs PostgreSQL server time (Cristian's algorithm).
TClockCalibration MeasureClockCalibration(const std::string& connectionString);

} // namespace NTpcc
