#pragma once

#include <workflows.h>
#include <context.h>

namespace NTpcc {

class TYdbTpccTransaction;

TFuture<bool> GetSimulationTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    TYdbTpccTransaction& tx);

} // namespace NTpcc
