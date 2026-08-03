#include "ob_capabilities.h"

namespace NTpcc {

TObCapabilities::TObCapabilities(std::string partitioningStyle, bool foreignKeys)
    : PartitioningStyle_(std::move(partitioningStyle))
    , ForeignKeys_(foreignKeys)
{
    if (PartitioningStyle_.empty()) {
        PartitioningStyle_ = "none";
    }
}

TCapabilities TObCapabilities::Get() const {
    TCapabilities c;
    c.IsolationLevels = {
        EIsolationLevel::RepeatableRead,
    };
    c.ExecuteBatchOptimized = false;
    c.ExecuteFinalAndCommitOptimized = false;
    c.AsyncDelivery = false;
    c.CancelSupported = true;
    c.MaxRecommendedInflight = 256;
    c.BulkLoadMechanism = "multi_insert";
    c.ExactDecimalType = "DECIMAL";
    c.ForeignKeys = ForeignKeys_;
    c.PartitioningStyle = PartitioningStyle_;
    return c;
}

} // namespace NTpcc
