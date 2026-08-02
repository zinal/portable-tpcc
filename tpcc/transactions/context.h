#pragma once

#include "error_classifier.h"
#include "session.h"

#include <task_queue.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace NTpcc {

extern std::atomic<size_t> TransactionsInflight;

struct TTransactionInflightGuard {
    TTransactionInflightGuard() {
        TransactionsInflight.fetch_add(1, std::memory_order_relaxed);
    }

    ~TTransactionInflightGuard() {
        TransactionsInflight.fetch_sub(1, std::memory_order_relaxed);
    }
};

struct TTransactionContext {
    size_t TerminalID = 0;
    size_t WarehouseID = 0;
    // Home district for this terminal (TPC-C §2.8.1.1 Stock-Level binding).
    // New-Order / Payment / Order-Status still sample D_ID randomly in [1..10].
    size_t DistrictID = 0;
    size_t WarehouseCount = 0;
    ITaskQueue& TaskQueue;

    int SimulateTransactionMs = 0;
    int SimulateTransactionSelect1 = 0;

    // Generated once per business transaction; reused across retry attempts.
    std::shared_ptr<void> FixedInputs;
};

template <typename TInputs, typename TGen>
const TInputs& FixedTransactionInputs(TTransactionContext& context, TGen&& gen) {
    if (!context.FixedInputs) {
        context.FixedInputs = std::make_shared<TInputs>(std::forward<TGen>(gen)());
    }
    return *std::static_pointer_cast<TInputs>(context.FixedInputs);
}

struct TUserAbortedException : public std::runtime_error {
    TUserAbortedException()
        : std::runtime_error("User aborted transaction (expected rollback)")
    {}
};

// Thrown by shared workflows when a semantic op / commit fails with a classified error.
// Terminal maps Class into the retry policy (no SQLSTATE needed).
class TClassifiedError : public std::runtime_error {
public:
    TClassifiedError(EErrorClass cls, std::string nativeCode, std::string message)
        : std::runtime_error(message.empty() ? "classified error" : message)
        , Class(cls)
        , NativeCode(std::move(nativeCode))
    {}

    EErrorClass Class = EErrorClass::Permanent;
    std::string NativeCode;
};

// TPC-C 2.5.2.2 / 2.6.2.2: median customer in c_first-ordered set (1-based n/2 rounded up).
inline std::optional<TCustomerRow> SelectCustomerByLastNameMedian(
    const std::vector<TCustomerRow>& customers)
{
    if (customers.empty()) {
        return std::nullopt;
    }
    size_t index = customers.size() / 2;
    if (customers.size() % 2 == 0 && index > 0) {
        --index;
    }
    return customers[index];
}

} // namespace NTpcc
