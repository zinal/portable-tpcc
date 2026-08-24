#include "ydb_error_classifier.h"

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status_codes.h>

#include <util/generic/strbuf.h>
#include <util/string/cast.h>

#include <optional>

namespace NTpcc {

namespace {

bool TryParseNumericStatus(std::string_view text, NYdb::EStatus& out) {
    if (text.empty()) {
        return false;
    }
    size_t value = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<size_t>(c - '0');
    }
    out = static_cast<NYdb::EStatus>(value);
    return true;
}

bool TryParseNamedStatus(std::string_view text, NYdb::EStatus& out) {
    return TryFromString(TStringBuf(text.data(), text.size()), out);
}

// Accepts "400060", "OVERLOADED", and the combined "OVERLOADED (400060)" form
// produced by YdbStatusCodeOf.
std::optional<NYdb::EStatus> TryParseYdbStatus(std::string_view nativeCode) {
    if (nativeCode.empty()) {
        return std::nullopt;
    }

    NYdb::EStatus status = NYdb::EStatus::STATUS_UNDEFINED;
    if (TryParseNumericStatus(nativeCode, status) || TryParseNamedStatus(nativeCode, status)) {
        return status;
    }

    if (nativeCode.back() == ')') {
        const auto open = nativeCode.rfind(" (");
        if (open != std::string_view::npos) {
            const auto name = nativeCode.substr(0, open);
            const auto number = nativeCode.substr(open + 2, nativeCode.size() - open - 3);
            if (TryParseNamedStatus(name, status) || TryParseNumericStatus(number, status)) {
                return status;
            }
        }
    }
    return std::nullopt;
}

EErrorClass ClassifyByMessage(std::string_view message) {
    if (message.find("ABORTED") != std::string_view::npos ||
        message.find("UNAVAILABLE") != std::string_view::npos ||
        message.find("OVERLOADED") != std::string_view::npos ||
        message.find("SESSION_BUSY") != std::string_view::npos)
    {
        return EErrorClass::RetryableAbort;
    }
    if (message.find("UNDETERMINED") != std::string_view::npos ||
        message.find("TRANSPORT") != std::string_view::npos ||
        message.find("CLIENT_") != std::string_view::npos)
    {
        return EErrorClass::NotCommitted;
    }
    return EErrorClass::Permanent;
}

} // namespace

std::string YdbStatusCodeOf(NYdb::EStatus status) {
    const auto number = std::to_string(static_cast<size_t>(status));
    try {
        // Same GENERATE_ENUM_SERIALIZATION mapping the YDB CLI uses via ToString(EStatus).
        std::string name(ToString(status));
        if (name.empty() || name == number) {
            return number;
        }
        return name + " (" + number + ")";
    } catch (const std::exception&) {
        return number;
    }
}

std::string YdbIssuesToString(const NYdb::TStatus& status) {
    const auto code = YdbStatusCodeOf(status.GetStatus());
    const auto issues = status.GetIssues().ToOneLineString();
    if (issues.empty()) {
        return code;
    }
    return code + ": " + issues;
}

EErrorClass TYdbErrorClassifier::Classify(std::string_view nativeCode, std::string_view message) const {
    if (const auto parsed = TryParseYdbStatus(nativeCode)) {
        return ClassifyStatus(NYdb::TStatus(*parsed, {}));
    }
    return ClassifyByMessage(message.empty() ? nativeCode : message);
}

EErrorClass TYdbErrorClassifier::ClassifyStatus(const NYdb::TStatus& status, bool duringCommit) const {
    switch (status.GetStatus()) {
        case NYdb::EStatus::SUCCESS:
            return EErrorClass::Permanent;
        case NYdb::EStatus::ABORTED:
        case NYdb::EStatus::UNAVAILABLE:
        case NYdb::EStatus::OVERLOADED:
        case NYdb::EStatus::TIMEOUT:
        case NYdb::EStatus::BAD_SESSION:
        case NYdb::EStatus::SESSION_EXPIRED:
        case NYdb::EStatus::SESSION_BUSY:
        case NYdb::EStatus::CLIENT_RESOURCE_EXHAUSTED:
        case NYdb::EStatus::CLIENT_LIMITS_REACHED:
            return EErrorClass::RetryableAbort;
        case NYdb::EStatus::TRANSPORT_UNAVAILABLE:
        case NYdb::EStatus::CLIENT_DEADLINE_EXCEEDED:
        case NYdb::EStatus::CLIENT_INTERNAL_ERROR:
            return duringCommit ? EErrorClass::AmbiguousCommit : EErrorClass::NotCommitted;
        case NYdb::EStatus::UNDETERMINED:
            return EErrorClass::AmbiguousCommit;
        case NYdb::EStatus::PRECONDITION_FAILED:
        case NYdb::EStatus::ALREADY_EXISTS:
            return EErrorClass::Integrity;
        case NYdb::EStatus::CANCELLED:
        case NYdb::EStatus::CLIENT_CANCELLED:
            return EErrorClass::Cancelled;
        default:
            return EErrorClass::Permanent;
    }
}

EErrorClass TYdbErrorClassifier::ClassifyException(const std::exception& ex) const {
    return ClassifyException(ex, false);
}

EErrorClass TYdbErrorClassifier::ClassifyException(const std::exception& ex, bool duringCommit) const {
    if (dynamic_cast<const NYdb::NStatusHelpers::TYdbErrorException*>(&ex)) {
        const auto& ydb = static_cast<const NYdb::NStatusHelpers::TYdbErrorException&>(ex);
        return ClassifyStatus(ydb.GetStatus(), duringCommit);
    }
    return duringCommit ? EErrorClass::AmbiguousCommit : Classify({}, ex.what());
}

} // namespace NTpcc
