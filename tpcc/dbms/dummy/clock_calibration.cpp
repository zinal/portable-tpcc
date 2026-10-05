#include "clock_calibration.h"

#include <chrono>

namespace NTpcc {

TClockCalibration MeasureClockCalibration() {
    TClockCalibration cal;
    cal.MeasuredAt = std::chrono::system_clock::now();
    cal.OffsetMs = 0;
    cal.UncertaintyMs = 0;
    cal.RttMs = 0;
    cal.TimeSource = "dummy";
    return cal;
}

} // namespace NTpcc
