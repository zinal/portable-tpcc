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
class IAdminAdapter {
public:
    virtual ~IAdminAdapter() = default;
    virtual void EnsureSchema() = 0;
    virtual void EnsureIndexes() = 0;
    virtual void EnsureStatistics() = 0;
    virtual void Clean() = 0;
    virtual TAdminDescribe Describe() = 0;
};

} // namespace NTpcc
