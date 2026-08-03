#pragma once

#include "ydb_driver.h"

#include <clock_skew.h>

#include <string>

namespace NTpcc {

// Estimate local clock offset vs YDB server time (Cristian's algorithm).
TClockCalibration MeasureClockCalibration(const TYdbConnectionConfig& connectionConfig,
                                          const std::string& timeSource = {});

} // namespace NTpcc
