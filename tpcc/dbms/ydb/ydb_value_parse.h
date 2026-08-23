#pragma once

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/value/value.h>

#include <cstdint>
#include <optional>
#include <string>

namespace NTpcc {

// YQL types SELECT columns as either T or Optional<T>. The SDK getters are
// not interchangeable: GetInt32() throws on Optional<Int32>, and
// GetOptionalInt32() throws on a non-optional Int32.

int32_t Int32FromValue(NYdb::TValueParser& parser);
uint64_t CountFromValue(NYdb::TValueParser& parser);
std::string Utf8FromValue(NYdb::TValueParser& parser);
NYdb::TDecimalValue DecimalFromValue(NYdb::TValueParser& parser);
std::optional<int32_t> OptionalInt32FromValue(NYdb::TValueParser& parser);
std::optional<TInstant> OptionalTimestampFromValue(NYdb::TValueParser& parser);

} // namespace NTpcc
