#pragma once

#include "catalog.h"
#include "types.h"

#include <string>

namespace NTpcc {

struct TCheckRequest {
    int WarehouseCount = 0;
    ECheckPhase Phase = ECheckPhase::AfterRun;
    // Parallel DBMS sessions for check queries. 1 = serial (default).
    int CheckConcurrency = 1;
    std::string Path; // DBMS schema / search_path qualifier
    std::string RunId;
    std::string Instance;
};

// Evaluates shared catalog entries against a live database.
class ICheckAdapter {
public:
    virtual ~ICheckAdapter() = default;
    virtual TCheckReport Run(const TCheckRequest& request) = 0;
};

} // namespace NTpcc
