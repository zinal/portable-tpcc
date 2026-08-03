#include "clock_skew.h"

#include <sstream>

namespace NTpcc {

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
