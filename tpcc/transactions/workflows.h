#pragma once

#include "context.h"
#include "session.h"

#include <future.h>

#include <chrono>

namespace NTpcc {

// Shared TPC-C workflows over ITpccTransaction (not PgSession).
TFuture<bool> GetNewOrderTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    ITpccTransaction& tx);

TFuture<bool> GetDeliveryTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    ITpccTransaction& tx);

TFuture<bool> GetOrderStatusTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    ITpccTransaction& tx);

TFuture<bool> GetPaymentTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    ITpccTransaction& tx);

TFuture<bool> GetStockLevelTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    ITpccTransaction& tx);

} // namespace NTpcc
