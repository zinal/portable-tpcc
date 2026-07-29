#include "error_classifier.h"

namespace NTpcc {

EErrorClass ClassifySqlState(std::string_view sqlState) {
    if (sqlState.size() < 2) {
        return EErrorClass::Permanent;
    }

    // Class 40 — transaction rollback
    if (sqlState == "40001" || sqlState == "40P01") {
        // serialization_failure / deadlock_detected
        return EErrorClass::RetryableAbort;
    }
    if (sqlState.substr(0, 2) == "40") {
        return EErrorClass::RetryableAbort;
    }

    // Class 08 — connection exception (often ambiguous if during commit)
    if (sqlState.substr(0, 2) == "08") {
        return EErrorClass::NotCommitted;
    }

    // Class 23 — integrity constraint violation
    if (sqlState.substr(0, 2) == "23") {
        return EErrorClass::Integrity;
    }

    // Query canceled (must precede class-57 catch-all)
    if (sqlState == "57014") {
        return EErrorClass::Cancelled;
    }

    // Class 53 / 57 — insufficient resources / operator intervention
    if (sqlState.substr(0, 2) == "53" || sqlState.substr(0, 2) == "57") {
        return EErrorClass::Permanent;
    }

    return EErrorClass::Permanent;
}

bool IsRetryable(EErrorClass c) {
    return c == EErrorClass::RetryableAbort || c == EErrorClass::NotCommitted;
}

bool MayBlindRetry(EErrorClass c) {
    // AmbiguousCommit / Integrity / Cancelled / Permanent MUST NOT be blind-retried.
    return IsRetryable(c);
}

} // namespace NTpcc
