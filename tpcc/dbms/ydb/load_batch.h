#pragma once

#include "ydb_driver.h"

#include <put_batch.h>

#include <cstdint>
#include <string>

namespace NTpcc {

// parameter-reference.md: data.batch_rows is 2000 when ≤ 0. The YDB loader
// must apply this itself: standalone import and a run-config with 0 otherwise
// send one BulkUpsert per table (100k stock rows), which can exceed the
// server 300s upload deadline.
constexpr int DEFAULT_YDB_LOAD_BATCH_ROWS = 2000;

inline int EffectiveYdbLoadBatchRows(int batchRows) {
    return batchRows > 0 ? batchRows : DEFAULT_YDB_LOAD_BATCH_ROWS;
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
