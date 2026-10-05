#include "dummy_capabilities.h"

namespace NTpcc {

TCapabilities TDummyCapabilities::Get() const {
    TCapabilities c;
    c.IsolationLevels = {
        EIsolationLevel::ReadCommitted,
        EIsolationLevel::RepeatableRead,
        EIsolationLevel::Serializable,
    };
    c.ExecuteBatchOptimized = false;
    c.ExecuteFinalAndCommitOptimized = false;
    c.AsyncDelivery = false;
    c.CancelSupported = true;
    c.MaxRecommendedInflight = 256;
    c.BulkLoadMechanism = "noop";
    c.ExactDecimalType = "DECIMAL";
    c.ForeignKeys = false;
    c.PartitioningStyle = "none";
    return c;
}

} // namespace NTpcc
