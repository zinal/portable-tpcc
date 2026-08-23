#pragma once

#include "ydb_driver.h"

#include <contrib/libs/apache/arrow/cpp/src/arrow/api.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/status/status.h>

#include <memory>
#include <string>

namespace NTpcc {

// Serialize Arrow IPC schema / record-batch payloads for BulkUpsert(ApacheArrow).
std::string SerializeArrowSchema(const arrow::Schema& schema);
std::string SerializeArrowBatch(const std::shared_ptr<arrow::RecordBatch>& batch);

// BulkUpsert is idempotent (specification §6). TIMEOUT / overload / transport
// failures are retried; the YDB SDK does not retry TIMEOUT by default.
bool ShouldRetryYdbBulkUpsert(const NYdb::TStatus& status);

// BulkUpsert using absolute table path and Apache Arrow payload.
void BulkUpsertArrow(
    TYdbConnection& connection,
    const std::string& table,
    const std::shared_ptr<arrow::RecordBatch>& batch);

} // namespace NTpcc
