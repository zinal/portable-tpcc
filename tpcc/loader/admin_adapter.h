#pragma once

#include <string>

namespace NTpcc {

struct TAdminDescribe {
    std::string AdapterName;
    std::string ServerVersion;
    std::string ClientVersion;
};

// Lifecycle of physical database objects for one workload path.
// No fence APIs: multi-host start sync uses wall-clock --start-at.
// EnsureSchema / EnsureIndexes / EnsureStatistics are separate stages
// (schema → load → indexes); each MUST be idempotent.
class IAdminAdapter {
public:
    virtual ~IAdminAdapter() = default;
    virtual void EnsureSchema() = 0;
    // Create workload access paths after bulk load. MUST be idempotent.
    virtual void EnsureIndexes() = 0;
    // Planner/tablet statistics after indexes. MUST be idempotent.
    virtual void EnsureStatistics() = 0;
    virtual void Clean() = 0;
    virtual TAdminDescribe Describe() = 0;
};

} // namespace NTpcc
