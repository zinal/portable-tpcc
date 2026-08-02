#include "clock_calibration.h"

#include <pqxx/pqxx>

#include <algorithm>
#include <chrono>
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

TSample TakeSample(pqxx::connection& conn) {
    const auto t0 = SysClock::now();
    pqxx::nontransaction tx(conn);
    const auto row = tx.exec1(
        "SELECT floor(extract(epoch from clock_timestamp()) * 1000)::bigint");
    const auto t1 = SysClock::now();

    const auto rttMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    const int64_t serverMs = row[0].as<int64_t>();
    const int64_t midpointMs = ToEpochMs(t0) + rttMs / 2;
    return TSample{serverMs - midpointMs, rttMs};
}

} // anonymous

TClockCalibration MeasureClockCalibration(const std::string& connectionString) {
    TClockCalibration result;
    result.MeasuredAt = SysClock::now();

    pqxx::connection conn(connectionString);
    std::vector<TSample> samples;
    samples.reserve(5);
    for (int i = 0; i < 5; ++i) {
        samples.push_back(TakeSample(conn));
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
