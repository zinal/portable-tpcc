#include "ydb_capabilities.h"

namespace NTpcc {

TCapabilities TYdbCapabilities::Get() const {
    TCapabilities c;
    c.IsolationLevels = {EIsolationLevel::Serializable};
    c.ExecuteBatchOptimized = false;
    c.ExecuteFinalAndCommitOptimized = true;
    c.AsyncDelivery = false;
    c.CancelSupported = true;
    c.MaxRecommendedInflight = 256;
    c.BulkLoadMechanism = "bulk_upsert";
    c.ExactDecimalType = "Decimal";
    c.ForeignKeys = false;
    c.PartitioningStyle = "warehouse_range";
    return c;
}

} // namespace NTpcc
