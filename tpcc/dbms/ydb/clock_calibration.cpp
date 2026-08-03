#include "clock_calibration.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace NTpcc {

namespace {

using SysClock = std::chrono::system_clock;

int64_t ToEpochMs(SysClock::time_point tp) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
}

struct TSample {
    int64_t OffsetMs = 0;
    int64_t RttMs = 0;
};

TSample TakeSample(TYdbConnection& connection) {
    const auto t0 = SysClock::now();
    auto result = connection.QueryClient().RetryQuery([](NYdb::NQuery::TSession session) {
        return session.ExecuteQuery(
            "SELECT CAST(CurrentUtcTimestamp() AS Int64) / 1000 AS server_ms;",
            NYdb::NQuery::TTxControl::NoTx());
    }).GetValueSync();
    const auto t1 = SysClock::now();
    if (!result.IsSuccess()) {
        throw std::runtime_error(result.GetIssues().ToOneLineString());
    }
    NYdb::TResultSetParser parser(result.GetResultSet(0));
    if (!parser.TryNextRow()) {
        throw std::runtime_error("clock calibration query returned no rows");
    }

    const auto rttMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const int64_t serverMs = parser.ColumnParser("server_ms").GetInt64();
    const int64_t midpointMs = ToEpochMs(t0) + rttMs / 2;
    return TSample{serverMs - midpointMs, rttMs};
}

} // anonymous

TClockCalibration MeasureClockCalibration(const TYdbConnectionConfig& connectionConfig,
                                          const std::string& timeSource) {
    TClockCalibration result;
    result.MeasuredAt = SysClock::now();
    result.TimeSource = timeSource;

    TYdbConnection connection(connectionConfig);
    std::vector<TSample> samples;
    samples.reserve(5);
    for (int i = 0; i < 5; ++i) {
        samples.push_back(TakeSample(connection));
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

namespace {

int64_t AbsInt64(int64_t v) {
    return v < 0 ? -v : v;
}

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

} // namespace NTpcc
