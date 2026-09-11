#include "ob_error_classifier.h"

#include <cstdlib>

namespace NTpcc {

namespace {

int ParseCode(std::string_view nativeCode) {
    if (nativeCode.empty()) {
        return 0;
    }
    return std::atoi(std::string(nativeCode).c_str());
}

EErrorClass ClassifyCode(int code, bool commit, std::string_view message = {}) {
    switch (ClassifyDbError(code, message)) {
        case EObDbErrorKind::Deadlock:
        case EObDbErrorKind::LockWaitTimeout:
        case EObDbErrorKind::SerializationFailure:
        case EObDbErrorKind::TransactionInvalidated:
            return EErrorClass::RetryableAbort;
        case EObDbErrorKind::Shutdown:
            return EErrorClass::Cancelled;
        case EObDbErrorKind::ConnectionLost:
            return commit ? EErrorClass::AmbiguousCommit : EErrorClass::NotCommitted;
        case EObDbErrorKind::TenantMemoryLimit:
        case EObDbErrorKind::Other:
            return EErrorClass::Permanent;
    }
    return EErrorClass::Permanent;
}

} // namespace

std::string ObNativeCodeOf(const std::exception& ex) {
    if (const auto* db = dynamic_cast<const TObDbError*>(&ex)) {
        return std::to_string(db->Code());
    }
    return {};
}

EErrorClass TObErrorClassifier::Classify(
    std::string_view nativeCode,
    std::string_view message) const
{
    return ClassifyCode(ParseCode(nativeCode), false, message);
}

EErrorClass TObErrorClassifier::ClassifyException(const std::exception& ex) const {
    if (const auto* db = dynamic_cast<const TObDbError*>(&ex)) {
        return ClassifyCode(db->Code(), false, db->what());
    }
    return EErrorClass::Permanent;
}

EErrorClass TObErrorClassifier::ClassifyCommitException(const std::exception& ex) const {
    if (const auto* db = dynamic_cast<const TObDbError*>(&ex)) {
        return ClassifyCode(db->Code(), true, db->what());
    }
    return EErrorClass::Permanent;
}

} // namespace NTpcc
