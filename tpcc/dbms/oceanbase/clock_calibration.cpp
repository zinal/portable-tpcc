#include "clock_calibration.h"

#include "ob_connection.h"

namespace NTpcc {

TClockCalibration MeasureClockCalibration(
    const std::string& connectionString,
    const std::string& timeSource)
{
    auto conn = TObConnection::Connect(ParseConnectionString(connectionString));
    return MeasureClockCalibrationWithSampler(
        [&conn]() -> int64_t {
            auto row = conn->QuerySimple(
                "SELECT CAST(UNIX_TIMESTAMP(CURRENT_TIMESTAMP(3)) * 1000 AS SIGNED) AS ts");
            if (!row.TryNextRow()) {
                throw std::runtime_error("clock query returned no rows");
            }
            return row.GetInt64("ts");
        },
        timeSource);
}

} // namespace NTpcc
