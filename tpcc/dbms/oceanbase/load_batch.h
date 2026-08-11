#pragma once

#include "ob_connection.h"

#include <put_batch.h>

#include <cstdint>
#include <string>

namespace NTpcc {

// Idempotent population helpers (specification §6):
// - item: INSERT ... ON DUPLICATE KEY UPDATE
// - warehouse range: INSERT first; on ERROR 1062 (ER_DUP_ENTRY), DELETE
//   warehouse-scoped tables and INSERT again
// Load path mirrors tpcc-oceanbase-cpp: multi-row INSERT batches of at most 200
// rows and a short transaction per table (not one warehouse-sized TX).

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
