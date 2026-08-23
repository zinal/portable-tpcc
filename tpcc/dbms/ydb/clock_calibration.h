#pragma once

#include "ydb_driver.h"

#include <clock_skew.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <cstdint>
#include <string>

namespace NTpcc {

// Convert a CurrentUtcTimestamp() column to epoch milliseconds.
// YQL may type the column as Timestamp or Optional<Timestamp>.
int64_t ServerEpochMsFromTimestampValue(NYdb::TValueParser& parser);

// Estimate local clock offset vs YDB server time (Cristian's algorithm).
TClockCalibration MeasureClockCalibration(const TYdbConnectionConfig& connectionConfig,
                                          const std::string& timeSource = {});

} // namespace NTpcc
