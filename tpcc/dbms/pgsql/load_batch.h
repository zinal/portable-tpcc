#pragma once

#include <put_batch.h>

#include <pqxx/pqxx>

#include <cstdint>
#include <string>

namespace NTpcc {

// Idempotent population helpers (specification §6):
// - item: staging COPY + INSERT ... ON CONFLICT DO UPDATE
// - warehouse range: COPY first; on unique_violation / SQLSTATE 23505,
//   DELETE warehouse-scoped tables and COPY again in one transaction

TPutBatchResult PutItemsIdempotent(
    pqxx::connection& conn,
    uint64_t seed,
    const std::string& runId = {},
    int batchRows = 0);

// Loads all data owned by a single warehouse id (warehouse, district, stock,
// customer, history, oorder, new_order, order_line). On primary-key conflict
// (duplicate key), deletes that warehouse range and reloads.
// When batchRows > 0, large COPY streams are flushed every batchRows rows.
TPutBatchResult PutWarehouseIdempotent(
    pqxx::connection& conn,
    uint64_t seed,
    int warehouseId,
    const std::string& runId = {},
    int batchRows = 0);

// Adapter façade over the helpers above. For table "item", Begin/End are ignored
// (full item population). For table "warehouse", [Begin, End) are warehouse ids.
// Row payloads are currently ignored: the adapter regenerates deterministic rows
// from Seed_ (shared generator). Non-empty rows are rejected until the shared
// loader serialisation lands.
class TPgLoadAdapter : public ILoadAdapter {
public:
    TPgLoadAdapter(pqxx::connection& conn, uint64_t seed);

    TPutBatchResult PutBatch(
        const std::string& runId,
        const TLoadKeyRange& keyRange,
        const std::vector<std::string>& rows) override;

private:
    pqxx::connection& Conn_;
    uint64_t Seed_ = 1;
};

} // namespace NTpcc
