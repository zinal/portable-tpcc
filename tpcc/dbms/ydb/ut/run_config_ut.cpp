#include <gtest/gtest.h>

#include <run_config.h>
#include <run_config_document.h>
#include <ydb_tx_mode.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>

using namespace NTpcc;

namespace {

std::string WriteRunConfig(const std::string& path, const std::string& optionsJson) {
    std::ofstream out(path, std::ios::trunc);
    out << R"({
  "run_id": "run-1",
  "database": {
    "dbms": "ydb",
    "endpoint": "localhost:2136",
    "database": "/local",
    "path": "tpcc")"
        << optionsJson << R"(
  },
  "scale": { "warehouses": 1 },
  "phases": { "measurement_ms": 1000 },
  "worker_assignment": [
    {
      "instance": "worker-a",
      "host": "localhost",
      "warehouse_ranges": [[1, 2]],
      "threads": 1,
      "max_inflight": 8
    }
  ]
}
)";
    return path;
}

} // namespace

TEST(YdbRunConfig, DefaultsTxModeWhenOptionsOmitted) {
    const std::string path = "ydb_run_config_ut_default.json";
    WriteRunConfig(path, "");
    const auto doc = LoadRunConfigDocument(path);
    EXPECT_TRUE(doc.TxMode.empty());
    EIsolationLevel isolation = EIsolationLevel::Serializable;
    ASSERT_TRUE(ParseYdbTxMode(doc.TxMode, isolation));
    EXPECT_EQ(isolation, EIsolationLevel::RepeatableRead);
    std::remove(path.c_str());
}

TEST(YdbRunConfig, ParsesSnapshotAndSerializableTxMode) {
    const std::string snapshotPath = "ydb_run_config_ut_snapshot.json";
    WriteRunConfig(snapshotPath, R"(,
    "options": { "tx_mode": "snapshot-rw" })");
    auto doc = LoadRunConfigDocument(snapshotPath);
    EXPECT_EQ(doc.TxMode, "snapshot-rw");
    std::remove(snapshotPath.c_str());

    const std::string serialPath = "ydb_run_config_ut_serial.json";
    WriteRunConfig(serialPath, R"(,
    "options": { "tx_mode": "serializable-rw" })");
    doc = LoadRunConfigDocument(serialPath);
    EXPECT_EQ(doc.TxMode, "serializable-rw");
    std::remove(serialPath.c_str());
}

TEST(YdbRunConfig, RejectsUnknownTxModeAndUnknownOptions) {
    const std::string badMode = "ydb_run_config_ut_bad_mode.json";
    WriteRunConfig(badMode, R"(,
    "options": { "tx_mode": "serializable" })");
    EXPECT_THROW(LoadRunConfigDocument(badMode), std::runtime_error);
    std::remove(badMode.c_str());

    const std::string unknown = "ydb_run_config_ut_unknown.json";
    WriteRunConfig(unknown, R"(,
    "options": { "partitioning": "none" })");
    EXPECT_THROW(LoadRunConfigDocument(unknown), std::runtime_error);
    std::remove(unknown.c_str());
}

TEST(YdbRunConfig, ParsesStatsIntervalMs) {
    const std::string path = "ydb_run_config_ut_stats_interval.json";
    {
        std::ofstream out(path, std::ios::trunc);
        out << R"({
  "run_id": "run-1",
  "database": {
    "dbms": "ydb",
    "endpoint": "localhost:2136",
    "database": "/local",
    "path": "tpcc"
  },
  "scale": { "warehouses": 1 },
  "phases": { "measurement_ms": 1000 },
  "runtime": { "stats_interval_ms": 5000 },
  "worker_assignment": [
    {
      "instance": "worker-a",
      "host": "localhost",
      "warehouse_ranges": [[1, 2]],
      "threads": 1,
      "max_inflight": 8
    }
  ]
}
)";
    }
    const auto doc = LoadRunConfigDocument(path);
    EXPECT_EQ(doc.StatsIntervalMs, 5000);
    std::remove(path.c_str());
}

TEST(YdbRunConfig, DefaultsStatsIntervalWhenRuntimeOmitted) {
    const std::string path = "ydb_run_config_ut_stats_default.json";
    WriteRunConfig(path, "");
    const auto doc = LoadRunConfigDocument(path);
    EXPECT_EQ(doc.StatsIntervalMs, 0);
    std::remove(path.c_str());
}
