#include "clock_calibration.h"

#include <stdexcept>

namespace NTpcc {

TClockCalibration MeasureClockCalibration(const TYdbConnectionConfig& connectionConfig,
                                          const std::string& timeSource) {
    TYdbConnection connection(connectionConfig);
    return MeasureClockCalibrationWithSampler(
        [&connection]() -> int64_t {
            auto result = connection.QueryClient().RetryQuery([](NYdb::NQuery::TSession session) {
                return session.ExecuteQuery(
                    "SELECT CAST(CurrentUtcTimestamp() AS Int64) / 1000 AS server_ms;",
                    NYdb::NQuery::TTxControl::NoTx());
            }).GetValueSync();
            if (!result.IsSuccess()) {
                throw std::runtime_error(result.GetIssues().ToOneLineString());
            }
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            if (!parser.TryNextRow()) {
                throw std::runtime_error("clock calibration query returned no rows");
            }
            return parser.ColumnParser("server_ms").GetInt64();
        },
        timeSource);
}

} // namespace NTpcc
