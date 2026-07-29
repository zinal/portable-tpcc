#pragma once

#include "session.h"

#include <string>
#include <string_view>

namespace NTpcc {

class IErrorClassifier {
public:
    virtual ~IErrorClassifier() = default;

    // Map a native DBMS code (e.g. SQLSTATE) and optional message to EErrorClass.
    virtual EErrorClass Classify(
        std::string_view nativeCode,
        std::string_view message = {}) const = 0;
};

// Shared helper: classify by SQLSTATE class/code (usable by PG and compatible adapters).
EErrorClass ClassifySqlState(std::string_view sqlState);

bool IsRetryable(EErrorClass c);
bool MayBlindRetry(EErrorClass c); // false for AmbiguousCommit / Integrity / Cancelled

} // namespace NTpcc
