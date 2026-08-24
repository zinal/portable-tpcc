#include "clock_calibration.h"

#include "ydb_error_classifier.h"

#include <stdexcept>

namespace NTpcc {

int64_t ServerEpochMsFromTimestampValue(NYdb::TValueParser& parser) {
    // YQL may wrap CurrentUtcTimestamp() as Optional<Timestamp>. The previous
    // CAST(... AS Int64) / 1000 form was always Optional (CAST is fallible).
    if (parser.GetKind() == NYdb::TTypeParser::ETypeKind::Optional) {
        const auto ts = parser.GetOptionalTimestamp();
        if (!ts.has_value()) {
            throw std::runtime_error("clock calibration query returned null timestamp");
        }
        return static_cast<int64_t>(ts->MilliSeconds());
    }
    return static_cast<int64_t>(parser.GetTimestamp().MilliSeconds());
}

TClockCalibration MeasureClockCalibration(const TYdbConnectionConfig& connectionConfig,
                                          const std::string& timeSource) {
    TYdbConnection connection(connectionConfig);
    return MeasureClockCalibrationWithSampler(
        [&connection]() -> int64_t {
            auto result = connection.QueryClient().RetryQuery([](NYdb::NQuery::TSession session) {
                return session.ExecuteQuery(
                    "SELECT CurrentUtcTimestamp() AS server_ts;",
                    NYdb::NQuery::TTxControl::NoTx());
            }).GetValueSync();
            if (!result.IsSuccess()) {
                throw std::runtime_error(YdbIssuesToString(result));
            }
            NYdb::TResultSetParser parser(result.GetResultSet(0));
            if (!parser.TryNextRow()) {
                throw std::runtime_error("clock calibration query returned no rows");
            }
            return ServerEpochMsFromTimestampValue(parser.ColumnParser("server_ts"));
        },
        timeSource);
}

} // namespace NTpcc
