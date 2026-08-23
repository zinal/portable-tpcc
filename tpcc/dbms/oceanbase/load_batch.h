#pragma once

#include "ob_connection.h"

#include <put_batch.h>

#include <algorithm>
#include <cstdint>
#include <string>

namespace NTpcc {

// Hard MySQL / OceanBase prepared-statement limit (ER_PS_MANY_PARAM / 1390).
constexpr size_t MaxPreparedPlaceholders = 65535;

inline size_t MaxRowsForColumns(size_t columnCount) {
    if (columnCount == 0) {
        return 1;
    }
    return std::max<size_t>(1, MaxPreparedPlaceholders / columnCount);
}

inline size_t EffectiveObLoadBatchRows(size_t columnCount, int batchRows) {
    return std::min(
        static_cast<size_t>(EffectiveLoadBatchRows(batchRows)),
        MaxRowsForColumns(columnCount));
}

// Idempotent population helpers (specification §6):
// - item: INSERT ... ON DUPLICATE KEY UPDATE
// - warehouse range: INSERT first; on ERROR 1062 (ER_DUP_ENTRY), DELETE
//   warehouse-scoped tables and INSERT again
// Load path: multi-row INSERT batches (DEFAULT_LOAD_BATCH_ROWS when
// batchRows ≤ 0) and a short transaction per table, capped by the prepared
// statement placeholder limit.

TPutBatchResult PutItemsIdempotent(
    TObConnection& conn,
    uint64_t seed,
    const std::string& runId = {},
    int batchRows = 0);

// Loads all data owned by a single warehouse id. On primary-key conflict
// (ERROR 1062), deletes that warehouse range and reloads.
TPutBatchResult PutWarehouseIdempotent(
    TObConnection& conn,
    uint64_t seed,
    int warehouseId,
    const std::string& runId = {},
    int batchRows = 0);

class TObLoadAdapter : public ILoadAdapter {
public:
    TObLoadAdapter(TObConnection& conn, uint64_t seed);

    TPutBatchResult PutBatch(
        const std::string& runId,
        const TLoadKeyRange& keyRange,
        const std::vector<std::string>& rows) override;

private:
    TObConnection& Conn_;
    uint64_t Seed_ = 1;
};

} // namespace NTpcc
