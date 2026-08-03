#pragma once

#include <clock_skew.h>

#include <string>

namespace NTpcc {

// Estimate local clock offset vs PostgreSQL server time (Cristian's algorithm).
TClockCalibration MeasureClockCalibration(const std::string& connectionString,
                                          const std::string& timeSource = {});

} // namespace NTpcc
