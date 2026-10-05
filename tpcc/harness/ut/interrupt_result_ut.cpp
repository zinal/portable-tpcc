#include <gtest/gtest.h>

#include <artifacts.h>
#include <run_loop.h>

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;

using namespace NTpcc;

TEST(InterruptResult, MeasurementSecondsUntilStop) {
    using clock = std::chrono::system_clock;
    const auto start = clock::time_point{std::chrono::seconds{1'700'000'000}};

    EXPECT_DOUBLE_EQ(MeasurementSecondsUntilStop(start, start), 0.0);
    EXPECT_DOUBLE_EQ(MeasurementSecondsUntilStop(start, start - std::chrono::seconds{5}), 0.0);
    EXPECT_DOUBLE_EQ(MeasurementSecondsUntilStop(clock::time_point{}, start), 0.0);
    EXPECT_NEAR(
        MeasurementSecondsUntilStop(start, start + std::chrono::milliseconds{1500}),
        1.5,
        1e-6);
}

TEST(InterruptResult, ResultJsonRecordsIncompletenessAndStopTime) {
    TRunConfigDocument doc;
    doc.RunId = "run-int";
    doc.RunConfigSha256 = "abc";

    TWorkerAssignment assign;
    assign.Instance = "worker-a";
    assign.Host = "host-a";
    assign.WarehouseRanges.push_back(TWarehouseRange{1, 2});
    assign.Threads = 1;
    assign.MaxInflight = 1;

    TTerminalStats stats(64, 256, false);
    const auto dir = fs::temp_directory_path() / "tpcc-interrupt-result-ut";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto paths = MakeArtifactPaths(dir.string());

    const auto start = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};
    const auto stopped = start + std::chrono::seconds{12};
    WriteWorkerResultJson(
        paths, doc, "worker-a", assign, stats, false,
        start, start, start + std::chrono::seconds{60}, start + std::chrono::seconds{70},
        12.0, 0, "nonce", "pgsql", "tpcc-pgsql",
        true, stopped);

    std::ifstream in(paths.ResultJson);
    ASSERT_TRUE(in.good());
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto parsed = nlohmann::json::parse(body);
    EXPECT_TRUE(parsed.at("incomplete").get<bool>());
    EXPECT_FALSE(parsed.at("stopped_at").get<std::string>().empty());
    EXPECT_DOUBLE_EQ(parsed.at("metrics").at("measurement_seconds").get<double>(), 12.0);
    EXPECT_EQ(parsed.at("exit_status").get<int>(), 0);

    fs::remove_all(dir);
}

TEST(InterruptResult, CompleteResultOmitsIncompleteFlag) {
    TRunConfigDocument doc;
    doc.RunId = "run-ok";
    doc.RunConfigSha256 = "abc";

    TWorkerAssignment assign;
    assign.Instance = "worker-a";
    assign.WarehouseRanges.push_back(TWarehouseRange{1, 2});

    TTerminalStats stats(64, 256, false);
    const auto dir = fs::temp_directory_path() / "tpcc-complete-result-ut";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const auto paths = MakeArtifactPaths(dir.string());
    const auto start = std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}};

    WriteWorkerResultJson(
        paths, doc, "worker-a", assign, stats, false,
        start, start, start + std::chrono::seconds{60}, start + std::chrono::seconds{70},
        60.0, 0, "nonce", "pgsql", "tpcc-pgsql");

    std::ifstream in(paths.ResultJson);
    ASSERT_TRUE(in.good());
    const std::string body((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto parsed = nlohmann::json::parse(body);
    EXPECT_FALSE(parsed.contains("incomplete"));
    EXPECT_FALSE(parsed.contains("stopped_at"));

    fs::remove_all(dir);
}
