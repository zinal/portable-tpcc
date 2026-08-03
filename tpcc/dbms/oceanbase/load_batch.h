#pragma once

#include "ob_connection.h"

#include <put_batch.h>

#include <cstdint>
#include <string>

namespace NTpcc {

TPutBatchResult PutItemsIdempotent(
    TObConnection& conn,
    uint64_t seed,
    const std::string& runId = {},
    int batchRows = 0);

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
