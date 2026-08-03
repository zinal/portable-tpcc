#pragma once

#include <workflows.h>
#include <context.h>

#include <future.h>

#include <chrono>

namespace NTpcc {

class TPgTpccTransaction;

// PG-only simulation path (SELECT 1 / sleep). Not part of the semantic session API.
TFuture<bool> GetSimulationTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    TPgTpccTransaction& tx);

} // namespace NTpcc
