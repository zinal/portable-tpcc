#pragma once

#include <exception>
#include <string>
#include <string_view>

namespace NTpcc {

enum class EObDbErrorKind {
    Other,
    Deadlock,
    LockWaitTimeout,
    SerializationFailure,
    TransactionInvalidated,
    ConnectionLost,
    TenantMemoryLimit,
    Shutdown,
};

inline int AbsObCode(int nativeCode) {
    return nativeCode < 0 ? -nativeCode : nativeCode;
}

// Observer OB_ALLOCATE_MEMORY_FAILED. SQL audit: failed,-4013,
// "No memory or reach tenant memory limit". Connector/C may surface 4013 or -4013.
inline bool IsTenantMemoryLimitCode(int nativeCode) {
    return AbsObCode(nativeCode) == 4013;
}

inline bool LooksLikeTenantMemoryLimit(std::string_view message) {
    return message.find("tenant memory") != std::string_view::npos
        || message.find("No memory or reach") != std::string_view::npos;
}

// Prefer the tenant-memory / observer code when Connector/C wraps it as a
// lost-connection (2013/2006/2027) after COM_STMT_PREPARE fails under OOM.
inline int PreferObNativeCode(
    int primaryCode,
    int secondaryCode = 0,
    std::string_view primaryMessage = {},
    std::string_view secondaryMessage = {})
{
    if (IsTenantMemoryLimitCode(primaryCode)) {
        return AbsObCode(primaryCode);
    }
    if (IsTenantMemoryLimitCode(secondaryCode)) {
        return AbsObCode(secondaryCode);
    }
    if (LooksLikeTenantMemoryLimit(primaryMessage)
        || LooksLikeTenantMemoryLimit(secondaryMessage))
    {
        return 4013;
    }
    return primaryCode != 0 ? primaryCode : secondaryCode;
}

// Client library codes that mean the MYSQL* handle is no longer usable.
// 2002/2003: connect failed; 2006/2013/2055: server gone/lost;
// 2014: protocol desync; 2027: malformed packet (common through OBProxy).
inline bool IsConnectionLostCode(int nativeCode) {
    switch (nativeCode) {
        case 2002:
        case 2003:
        case 2006:
        case 2013:
        case 2014:
        case 2027:
        case 2055:
            return true;
        default:
            return false;
    }
}

inline EObDbErrorKind ClassifyDbError(int nativeCode, std::string_view message = {}) {
    if (IsTenantMemoryLimitCode(nativeCode) || LooksLikeTenantMemoryLimit(message)) {
        return EObDbErrorKind::TenantMemoryLimit;
    }
    switch (nativeCode) {
        case 1213:
            return EObDbErrorKind::Deadlock;
        case 1205:
            return EObDbErrorKind::LockWaitTimeout;
        case 6235:
            return EObDbErrorKind::SerializationFailure;
        case 6002:
            return EObDbErrorKind::TransactionInvalidated;
        case 1317:
            return EObDbErrorKind::Shutdown;
        default:
            if (IsConnectionLostCode(nativeCode)) {
                return EObDbErrorKind::ConnectionLost;
            }
            return EObDbErrorKind::Other;
    }
}

inline bool IsRetryableTxError(EObDbErrorKind kind) {
    return kind == EObDbErrorKind::Deadlock
        || kind == EObDbErrorKind::LockWaitTimeout
        || kind == EObDbErrorKind::SerializationFailure
        || kind == EObDbErrorKind::TransactionInvalidated;
}

class TObDbError : public std::exception {
public:
    TObDbError(int code, std::string message)
        : Code_(code)
        , Kind_(ClassifyDbError(code, message))
        , Message_(std::move(message))
    {}

    const char* what() const noexcept override {
        return Message_.c_str();
    }

    int Code() const {
        return Code_;
    }

    EObDbErrorKind Kind() const {
        return Kind_;
    }

    bool Retryable() const {
        return IsRetryableTxError(Kind_);
    }

private:
    int Code_ = 0;
    EObDbErrorKind Kind_ = EObDbErrorKind::Other;
    std::string Message_;
};

} // namespace NTpcc
