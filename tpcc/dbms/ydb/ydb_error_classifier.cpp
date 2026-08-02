#include "ydb_error_classifier.h"

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status_codes.h>

#include <sstream>

namespace NTpcc {

std::string YdbStatusCodeOf(NYdb::EStatus status) {
    return std::to_string(static_cast<size_t>(status));
}

std::string YdbIssuesToString(const NYdb::TStatus& status) {
    return status.GetIssues().ToOneLineString();
}

EErrorClass TYdbErrorClassifier::Classify(std::string_view nativeCode, std::string_view message) const {
    if (nativeCode.empty()) {
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

    size_t value = 0;
    for (char c : nativeCode) {
        if (c < '0' || c > '9') {
            return Classify({}, message);
        }
        value = value * 10 + static_cast<size_t>(c - '0');
    }

    return ClassifyStatus(NYdb::TStatus(static_cast<NYdb::EStatus>(value), {}));
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

EErrorClass TYdbErrorClassifier::ClassifyException(const std::exception& ex, bool duringCommit) const {
    if (dynamic_cast<const NYdb::NStatusHelpers::TYdbErrorException*>(&ex)) {
        const auto& ydb = static_cast<const NYdb::NStatusHelpers::TYdbErrorException&>(ex);
        return ClassifyStatus(ydb.GetStatus(), duringCommit);
    }
    return duringCommit ? EErrorClass::AmbiguousCommit : Classify({}, ex.what());
}

} // namespace NTpcc
