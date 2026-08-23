#pragma once

#include "catalog.h"
#include "types.h"

#include <string>

namespace NTpcc {

struct TCheckRequest {
    int WarehouseCount = 0;
    ECheckPhase Phase = ECheckPhase::AfterTest;
    // Parallel DBMS sessions for check queries. 1 = serial (default).
    int CheckConcurrency = 1;
    std::string Path; // DBMS schema / search_path qualifier
    std::string RunId;
    std::string Instance;
};

// Inclusive warehouse-id chunk size for ranged TPC-C checks. Size 1 enables
// HASH partition pruning (OceanBase) and the same per-warehouse parallelism
// on PostgreSQL and YDB. SQL predicates are unchanged; only scheduling
// and the warehouse filter bounds change. Bounds (and other per-execution
// values) are bound parameters; the query text stays the same across chunks.
constexpr int kWarehouseCheckRange = 1;

// Evaluates shared catalog entries against a live database.
class ICheckAdapter {
public:
    virtual ~ICheckAdapter() = default;
    virtual TCheckReport Run(const TCheckRequest& request) = 0;
};

} // namespace NTpcc
