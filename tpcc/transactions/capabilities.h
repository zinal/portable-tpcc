#pragma once

#include "session.h"

#include <string>
#include <vector>

namespace NTpcc {

struct TCapabilities {
    std::vector<EIsolationLevel> IsolationLevels;
    bool ExecuteBatchOptimized = false;
    bool ExecuteFinalAndCommitOptimized = false;
    bool AsyncDelivery = false;
    bool CancelSupported = true;
    size_t MaxRecommendedInflight = 256;
    size_t MaxRecommendedSessions = 0;
    std::string BulkLoadMechanism; // "copy", "bulk_upsert", ...
    std::string ExactDecimalType;  // e.g. "DECIMAL"
    bool ForeignKeys = true;
    std::string PartitioningStyle;
};

class ICapabilities {
public:
    virtual ~ICapabilities() = default;
    virtual TCapabilities Get() const = 0;
};

} // namespace NTpcc
