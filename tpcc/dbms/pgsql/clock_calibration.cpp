#include "clock_calibration.h"

#include <pqxx/pqxx>

namespace NTpcc {

TClockCalibration MeasureClockCalibration(const std::string& connectionString,
                                          const std::string& timeSource) {
    pqxx::connection conn(connectionString);
    return MeasureClockCalibrationWithSampler(
        [&conn]() -> int64_t {
            pqxx::nontransaction tx(conn);
            const auto row = tx.exec1(
                "SELECT floor(extract(epoch from clock_timestamp()) * 1000)::bigint");
            return row[0].as<int64_t>();
        },
        timeSource);
}

} // namespace NTpcc
