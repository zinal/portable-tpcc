#pragma once

#include "ydb_driver.h"

#include <put_batch.h>

#include <cstdint>
#include <string>

namespace NTpcc {

// Standalone import / run-config 0 must still chunk BulkUpsert: one request
// per table (100k stock rows) can exceed the server 300s upload deadline.
constexpr int DEFAULT_YDB_LOAD_BATCH_ROWS = DEFAULT_LOAD_BATCH_ROWS;

inline int EffectiveYdbLoadBatchRows(int batchRows) {
    return EffectiveLoadBatchRows(batchRows);
}

// Idempotent population helpers (specification §6):
// BulkUpsert of deterministic rows by primary key. Retries overwrite the same
// keys; extra rows outside the initial key set are not deleted.

TPutBatchResult PutItemsIdempotent(
    TYdbConnection& connection,
    uint64_t seed,
    const std::string& runId = {},
    int batchRows = 0);

// Upserts all initial-population rows for a single warehouse id.
TPutBatchResult PutWarehouseIdempotent(
    TYdbConnection& connection,
    uint64_t seed,
    int warehouseId,
    const std::string& runId = {},
    int batchRows = 0);

class TYdbLoadAdapter : public ILoadAdapter {
public:
    TYdbLoadAdapter(TYdbConnection& connection, uint64_t seed);

    TPutBatchResult PutBatch(
        const std::string& runId,
        const TLoadKeyRange& keyRange,
        const std::vector<std::string>& rows) override;

private:
    TYdbConnection& Connection_;
    uint64_t Seed_ = 1;
};

} // namespace NTpcc
