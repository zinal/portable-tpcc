#pragma once

#include <clock_skew.h>

#include <string>

namespace NTpcc {

TClockCalibration MeasureClockCalibration(
    const std::string& connectionString,
    const std::string& timeSource = {});

} // namespace NTpcc
