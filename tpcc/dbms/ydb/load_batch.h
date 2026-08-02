#pragma once

#include "ydb_driver.h"

#include <put_batch.h>

#include <cstdint>
#include <string>

namespace NTpcc {

TPutBatchResult PutItemsIdempotent(
    TYdbConnection& connection,
    uint64_t seed,
    const std::string& runId = {},
    int batchRows = 0);

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
