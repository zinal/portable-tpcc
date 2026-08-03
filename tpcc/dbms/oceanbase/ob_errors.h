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
    Shutdown,
};

inline EObDbErrorKind ClassifyDbError(int nativeCode, std::string_view /*message*/ = {}) {
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
        case 2006:
        case 2013:
            return EObDbErrorKind::ConnectionLost;
        default:
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
