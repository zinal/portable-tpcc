#pragma once

#include "pg_session.h"

#include <context.h>
#include <workflows.h>

#include <future.h>

#include <chrono>

namespace NTpcc {

// PG-only simulation path (SELECT 1 / sleep). Not part of the semantic session API.
TFuture<bool> GetSimulationTask(
    TTransactionContext& context,
    std::chrono::microseconds& latency,
    PgSession& session);

} // namespace NTpcc
