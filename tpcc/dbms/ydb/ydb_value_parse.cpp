#include "ydb_value_parse.h"

#include <stdexcept>

namespace NTpcc {

namespace {

[[noreturn]] void ThrowNull(const char* what) {
    throw std::runtime_error(std::string("YDB column is null: ") + what);
}

[[noreturn]] void ThrowUnexpected(const char* what) {
    throw std::runtime_error(std::string("YDB column has unexpected type for ") + what);
}

uint64_t NonNegativeCount(int64_t value) {
    if (value < 0) {
        throw std::runtime_error("YDB count column is negative");
    }
    return static_cast<uint64_t>(value);
}

} // anonymous

int32_t Int32FromValue(NYdb::TValueParser& parser) {
    if (parser.GetKind() == NYdb::TTypeParser::ETypeKind::Optional) {
        const auto value = parser.GetOptionalInt32();
        if (!value.has_value()) {
            ThrowNull("Int32");
        }
        return *value;
    }
    if (parser.GetKind() != NYdb::TTypeParser::ETypeKind::Primitive ||
        parser.GetPrimitiveType() != NYdb::EPrimitiveType::Int32)
    {
        ThrowUnexpected("Int32");
    }
    return parser.GetInt32();
}

uint64_t CountFromValue(NYdb::TValueParser& parser) {
    if (parser.GetKind() == NYdb::TTypeParser::ETypeKind::Optional) {
        parser.OpenOptional();
        if (parser.IsNull()) {
            parser.CloseOptional();
            ThrowNull("count");
        }
        const uint64_t value = CountFromValue(parser);
        parser.CloseOptional();
        return value;
    }
    if (parser.GetKind() != NYdb::TTypeParser::ETypeKind::Primitive) {
        ThrowUnexpected("count");
    }
    switch (parser.GetPrimitiveType()) {
        case NYdb::EPrimitiveType::Uint64:
            return parser.GetUint64();
        case NYdb::EPrimitiveType::Uint32:
            return parser.GetUint32();
        case NYdb::EPrimitiveType::Int64:
            return NonNegativeCount(parser.GetInt64());
        case NYdb::EPrimitiveType::Int32:
            return NonNegativeCount(parser.GetInt32());
        default:
            ThrowUnexpected("count");
    }
}

std::string Utf8FromValue(NYdb::TValueParser& parser) {
    if (parser.GetKind() == NYdb::TTypeParser::ETypeKind::Optional) {
        const auto value = parser.GetOptionalUtf8();
        if (!value.has_value()) {
            ThrowNull("Utf8");
        }
        return std::string(*value);
    }
    if (parser.GetKind() != NYdb::TTypeParser::ETypeKind::Primitive ||
        parser.GetPrimitiveType() != NYdb::EPrimitiveType::Utf8)
    {
        ThrowUnexpected("Utf8");
    }
    return std::string(parser.GetUtf8());
}

NYdb::TDecimalValue DecimalFromValue(NYdb::TValueParser& parser) {
    if (parser.GetKind() == NYdb::TTypeParser::ETypeKind::Optional) {
        const auto value = parser.GetOptionalDecimal();
        if (!value.has_value()) {
            ThrowNull("Decimal");
        }
        return *value;
    }
    if (parser.GetKind() != NYdb::TTypeParser::ETypeKind::Decimal) {
        ThrowUnexpected("Decimal");
    }
    return parser.GetDecimal();
}

std::optional<int32_t> OptionalInt32FromValue(NYdb::TValueParser& parser) {
    if (parser.GetKind() == NYdb::TTypeParser::ETypeKind::Optional) {
        return parser.GetOptionalInt32();
    }
    if (parser.GetKind() != NYdb::TTypeParser::ETypeKind::Primitive ||
        parser.GetPrimitiveType() != NYdb::EPrimitiveType::Int32)
    {
        ThrowUnexpected("Optional<Int32>");
    }
    return parser.GetInt32();
}

std::optional<TInstant> OptionalTimestampFromValue(NYdb::TValueParser& parser) {
    if (parser.GetKind() == NYdb::TTypeParser::ETypeKind::Optional) {
        return parser.GetOptionalTimestamp();
    }
    if (parser.GetKind() != NYdb::TTypeParser::ETypeKind::Primitive ||
        parser.GetPrimitiveType() != NYdb::EPrimitiveType::Timestamp)
    {
        ThrowUnexpected("Optional<Timestamp>");
    }
    return parser.GetTimestamp();
}

} // namespace NTpcc
