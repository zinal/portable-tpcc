#include <prometheus_export.h>
#include <terminal.h>

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace NTpcc {
namespace {

std::string HttpGet(int port, const char* path) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    const std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    EXPECT_GT(::send(fd, req.data(), req.size(), 0), 0);
    std::string out;
    char buf[1024];
    for (;;) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return out;
}

TTerminalStats::TLatencySample SampleMs(int ms) {
    TTerminalStats::TLatencySample sample;
    // Live histograms store Full the same way the terminal does: microseconds.
    sample.Full = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::milliseconds(ms));
    sample.AdmissionWait = std::chrono::microseconds(100);
    return sample;
}

} // namespace

TEST(PrometheusExport, RendersCumulativeCounters) {
    TPromSnapshot snap;
    snap.Inflight = 7;
    auto& no = snap.Tx[static_cast<size_t>(ETransactionType::NewOrder)];
    no.Success = 6;
    no.Failure = 1;
    no.Retries = 2;
    no.Rollbacks = 1;
    no.SuccessLatency.RecordMicros(5'000'000);
    no.FailureLatency.RecordMicros(1'000);
    no.AdmissionWait.RecordMicros(2000);
    no.SessionPoolWait.RecordMicros(500);
    no.RetryBackoff.RecordMicros(10'000);

    const std::string text = RenderPrometheusMetrics(snap);
    EXPECT_NE(text.find("# TYPE tpcc_transactions_total counter"), std::string::npos);
    EXPECT_NE(text.find("# TYPE tpcc_transaction_retries_total counter"), std::string::npos);
    EXPECT_NE(text.find("# TYPE tpcc_transaction_rollbacks_total counter"), std::string::npos);
    EXPECT_NE(text.find("# TYPE tpcc_inflight gauge"), std::string::npos);
    EXPECT_NE(text.find("# TYPE tpcc_transaction_duration_seconds histogram"), std::string::npos);
    EXPECT_NE(text.find("tpcc_inflight 7"), std::string::npos);
    EXPECT_NE(text.find("tpcc_transactions_total{type=\"new_order\",result=\"success\"} 6"), std::string::npos);
    EXPECT_NE(text.find("tpcc_transactions_total{type=\"new_order\",result=\"failure\"} 1"), std::string::npos);
    EXPECT_NE(text.find("tpcc_transaction_retries_total{type=\"new_order\"} 2"), std::string::npos);
    EXPECT_NE(text.find("tpcc_transaction_rollbacks_total{type=\"new_order\"} 1"), std::string::npos);
    EXPECT_NE(text.find("tpcc_transaction_duration_seconds_bucket{type=\"new_order\",result=\"success\",le=\"5\"} 1"), std::string::npos);
    EXPECT_NE(text.find("tpcc_transaction_duration_seconds_bucket{type=\"new_order\",result=\"success\",le=\"+Inf\"} 1"), std::string::npos);
    EXPECT_NE(text.find("tpcc_transaction_duration_seconds_sum{type=\"new_order\",result=\"success\"} 5.000000"), std::string::npos);
    EXPECT_NE(text.find("tpcc_transaction_duration_seconds_count{type=\"new_order\",result=\"failure\"} 1"), std::string::npos);
    EXPECT_NE(text.find("tpcc_client_wait_seconds_bucket{type=\"new_order\",wait=\"admission\",le=\"0.002\"} 1"), std::string::npos);
    EXPECT_NE(text.find("tpcc_client_wait_seconds_bucket{type=\"new_order\",wait=\"session_pool\",le=\"0.0005\"} 1"), std::string::npos);
    EXPECT_NE(text.find("tpcc_client_wait_seconds_bucket{type=\"new_order\",wait=\"retry_backoff\",le=\"0.01\"} 1"), std::string::npos);
    EXPECT_EQ(text.find("tpcc_tpmc"), std::string::npos);
    EXPECT_EQ(text.find("tpcc_collection_interval_seconds"), std::string::npos);
    EXPECT_EQ(text.find("# TYPE tpcc_transactions "), std::string::npos);
}

TEST(PrometheusExport, LiveMetricsAccumulate) {
    TTerminalStats stats;
    stats.EnableLiveMetrics();
    stats.RecordLiveSuccess(ETransactionType::NewOrder, SampleMs(5), true);
    stats.RecordLiveFailure(ETransactionType::Payment, SampleMs(5));
    stats.RecordLiveRetry(ETransactionType::NewOrder);

    std::array<TPromTxSnapshot, TRANSACTION_TYPE_COUNT> first{};
    stats.CopyLiveMetrics(first);
    EXPECT_EQ(first[static_cast<size_t>(ETransactionType::NewOrder)].Success, 1u);
    EXPECT_EQ(first[static_cast<size_t>(ETransactionType::NewOrder)].Rollbacks, 1u);
    EXPECT_EQ(first[static_cast<size_t>(ETransactionType::NewOrder)].Retries, 1u);
    EXPECT_EQ(first[static_cast<size_t>(ETransactionType::NewOrder)].SuccessLatency.Count(), 1u);
    EXPECT_EQ(first[static_cast<size_t>(ETransactionType::Payment)].Failure, 1u);

    std::array<TPromTxSnapshot, TRANSACTION_TYPE_COUNT> second{};
    stats.CopyLiveMetrics(second);
    EXPECT_EQ(second[static_cast<size_t>(ETransactionType::NewOrder)].Success, 1u);
    EXPECT_EQ(second[static_cast<size_t>(ETransactionType::Payment)].Failure, 1u);
    EXPECT_EQ(second[static_cast<size_t>(ETransactionType::NewOrder)].SuccessLatency.Count(), 1u);

    stats.RecordLiveSuccess(ETransactionType::NewOrder, SampleMs(5), false);
    std::array<TPromTxSnapshot, TRANSACTION_TYPE_COUNT> third{};
    stats.CopyLiveMetrics(third);
    EXPECT_EQ(third[static_cast<size_t>(ETransactionType::NewOrder)].Success, 2u);
    EXPECT_EQ(third[static_cast<size_t>(ETransactionType::NewOrder)].Rollbacks, 1u);
    EXPECT_EQ(third[static_cast<size_t>(ETransactionType::NewOrder)].SuccessLatency.Count(), 2u);
    EXPECT_EQ(third[static_cast<size_t>(ETransactionType::NewOrder)].SuccessLatency.SumMicros(), 10'000u);
}

TEST(PrometheusExport, HttpMetrics) {
    std::vector<std::shared_ptr<TTerminalStats>> perThread;
    auto stats = std::make_shared<TTerminalStats>();
    stats->EnableLiveMetrics();
    perThread.push_back(stats);

    TPrometheusExporter exporter(0, perThread);
    ASSERT_GT(exporter.Port(), 0);
    stats->RecordLiveSuccess(ETransactionType::NewOrder, SampleMs(5), false);

    const std::string ok = HttpGet(exporter.Port(), "/metrics");
    EXPECT_NE(ok.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(ok.find("text/plain; version=0.0.4"), std::string::npos);
    EXPECT_NE(ok.find("tpcc_transactions_total{type=\"new_order\",result=\"success\"} 1"), std::string::npos);
    EXPECT_NE(ok.find("# TYPE tpcc_transactions_total counter"), std::string::npos);

    stats->RecordLiveSuccess(ETransactionType::NewOrder, SampleMs(5), false);
    const std::string again = HttpGet(exporter.Port(), "/metrics");
    EXPECT_NE(again.find("tpcc_transactions_total{type=\"new_order\",result=\"success\"} 2"), std::string::npos);
    EXPECT_NE(again.find("tpcc_transaction_duration_seconds_count{type=\"new_order\",result=\"success\"} 2"), std::string::npos);

    const std::string missing = HttpGet(exporter.Port(), "/nope");
    EXPECT_NE(missing.find("HTTP/1.1 404"), std::string::npos);
}

} // namespace NTpcc
