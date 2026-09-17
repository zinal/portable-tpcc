#pragma once

#include <session.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/tx.h>

#include <string>

namespace NTpcc {

// Profile / CLI names match ydb workload tpcc --tx-mode (PR ydb-platform/ydb#31468).
// snapshot-rw is YDB snapshot isolation (Repeatable Read analogue) and the default.
inline constexpr const char* YDB_TX_MODE_SNAPSHOT_RW = "snapshot-rw";
inline constexpr const char* YDB_TX_MODE_SERIALIZABLE_RW = "serializable-rw";
inline constexpr const char* YDB_DEFAULT_TX_MODE = YDB_TX_MODE_SNAPSHOT_RW;

bool ParseYdbTxMode(const std::string& value, EIsolationLevel& out);
const char* YdbTxModeName(EIsolationLevel isolation);
NYdb::NQuery::TTxSettings YdbTxSettingsForIsolation(EIsolationLevel isolation);

} // namespace NTpcc
