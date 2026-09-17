#include "ydb_tx_mode.h"

namespace NTpcc {

bool ParseYdbTxMode(const std::string& value, EIsolationLevel& out) {
    if (value.empty() || value == YDB_TX_MODE_SNAPSHOT_RW) {
        out = EIsolationLevel::RepeatableRead;
        return true;
    }
    if (value == YDB_TX_MODE_SERIALIZABLE_RW) {
        out = EIsolationLevel::Serializable;
        return true;
    }
    return false;
}

const char* YdbTxModeName(EIsolationLevel isolation) {
    if (isolation == EIsolationLevel::Serializable) {
        return YDB_TX_MODE_SERIALIZABLE_RW;
    }
    return YDB_TX_MODE_SNAPSHOT_RW;
}

NYdb::NQuery::TTxSettings YdbTxSettingsForIsolation(EIsolationLevel isolation) {
    if (isolation == EIsolationLevel::Serializable) {
        return NYdb::NQuery::TTxSettings::SerializableRW();
    }
    return NYdb::NQuery::TTxSettings::SnapshotRW();
}

} // namespace NTpcc
