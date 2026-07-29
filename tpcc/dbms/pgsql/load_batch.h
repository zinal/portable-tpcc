#pragma once

#include <put_batch.h>

#include <pqxx/pqxx>

#include <cstdint>
#include <string>

namespace NTpcc {

// Idempotent population helpers (specification §6):
// - item: staging COPY + INSERT ... ON CONFLICT DO UPDATE
// - warehouse range: DELETE warehouse (CASCADE children) + COPY in FK order,
//   all in one transaction per warehouse id

TPutBatchResult PutItemsIdempotent(
    pqxx::connection& conn,
    uint64_t seed,
    const std::string& runId = {});

// Replaces all data owned by a single warehouse id (warehouse, district, stock,
// customer, history, oorder, new_order, order_line).
TPutBatchResult PutWarehouseIdempotent(
    pqxx::connection& conn,
    uint64_t seed,
    int warehouseId,
    const std::string& runId = {});

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
