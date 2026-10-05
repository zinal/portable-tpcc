#include <gtest/gtest.h>

#include <run_config.h>
#include <run_config_document.h>

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
    "dbms": "dummy",
    "endpoint": "localhost")"
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

TEST(DummyRunConfig, DefaultsDelayWhenOptionsOmitted) {
    const std::string path = "dummy_run_config_ut_default.json";
    WriteRunConfig(path, "");
    const auto doc = LoadRunConfigDocument(path);
    EXPECT_EQ(doc.Dbms, "dummy");
    EXPECT_EQ(doc.DelayUsMin, 0);
    EXPECT_EQ(doc.DelayUsMax, 0);
    std::remove(path.c_str());
}

TEST(DummyRunConfig, ParsesDelayRange) {
    const std::string path = "dummy_run_config_ut_delay.json";
    WriteRunConfig(path, R"(,
    "options": { "delay_us_min": 100, "delay_us_max": 400 })");
    const auto doc = LoadRunConfigDocument(path);
    EXPECT_EQ(doc.DelayUsMin, 100);
    EXPECT_EQ(doc.DelayUsMax, 400);
    const auto delay = DelayConfigFromDocument(doc);
    EXPECT_EQ(delay.MinUs, 100);
    EXPECT_EQ(delay.MaxUs, 400);
    std::remove(path.c_str());
}

TEST(DummyRunConfig, MinAloneCopiesToMax) {
    const std::string path = "dummy_run_config_ut_min_only.json";
    WriteRunConfig(path, R"(,
    "options": { "delay_us_min": 25 })");
    const auto doc = LoadRunConfigDocument(path);
    EXPECT_EQ(doc.DelayUsMin, 25);
    EXPECT_EQ(doc.DelayUsMax, 25);
    std::remove(path.c_str());
}

TEST(DummyRunConfig, RejectsUnknownOptionsAndInvertedRange) {
    const std::string unknown = "dummy_run_config_ut_unknown.json";
    WriteRunConfig(unknown, R"(,
    "options": { "partitioning": "none" })");
    EXPECT_THROW(LoadRunConfigDocument(unknown), std::runtime_error);
    std::remove(unknown.c_str());

    const std::string inverted = "dummy_run_config_ut_inverted.json";
    WriteRunConfig(inverted, R"(,
    "options": { "delay_us_min": 50, "delay_us_max": 10 })");
    EXPECT_THROW(LoadRunConfigDocument(inverted), std::runtime_error);
    std::remove(inverted.c_str());
}

TEST(DummyRunConfig, RejectsWrongDbms) {
    const std::string path = "dummy_run_config_ut_pgsql.json";
    std::ofstream out(path, std::ios::trunc);
    out << R"({
  "run_id": "run-1",
  "database": { "dbms": "pgsql", "endpoint": "localhost" },
  "scale": { "warehouses": 1 },
  "phases": { "measurement_ms": 1000 },
  "worker_assignment": [
    { "instance": "worker-a", "host": "localhost", "warehouse_ranges": [[1, 2]], "threads": 1, "max_inflight": 8 }
  ]
}
)";
    out.close();
    EXPECT_THROW(LoadRunConfigDocument(path), std::runtime_error);
    std::remove(path.c_str());
}
