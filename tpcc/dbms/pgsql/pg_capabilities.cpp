#include "pg_capabilities.h"

namespace NTpcc {

TPgCapabilities::TPgCapabilities(std::string partitioningStyle)
    : PartitioningStyle_(std::move(partitioningStyle))
{
    if (PartitioningStyle_.empty()) {
        PartitioningStyle_ = "none";
    }
}

TCapabilities TPgCapabilities::Get() const {
    TCapabilities c;
    c.IsolationLevels = {
        EIsolationLevel::RepeatableRead,
    };
    c.ExecuteBatchOptimized = false;
    c.ExecuteFinalAndCommitOptimized = false;
    c.AsyncDelivery = false; // decision F
    c.CancelSupported = true;
    c.MaxRecommendedInflight = 256;
    c.BulkLoadMechanism = "copy";
    c.ExactDecimalType = "DECIMAL";
    c.ForeignKeys = true;
    c.PartitioningStyle = PartitioningStyle_;
    return c;
}

} // namespace NTpcc
