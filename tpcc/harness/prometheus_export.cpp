#include "prometheus_export.h"

#include <log.h>
#include <terminal.h>
#include <context.h>

#include <fmt/format.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace NTpcc {

namespace {

const char* PrometheusTypeLabel(size_t index) {
    switch (static_cast<ETransactionType>(index)) {
        case ETransactionType::NewOrder: return "new_order";
        case ETransactionType::Delivery: return "delivery";
        case ETransactionType::OrderStatus: return "order_status";
        case ETransactionType::Payment: return "payment";
        case ETransactionType::StockLevel: return "stock_level";
        default: return "unknown";
    }
}

void AppendHelp(std::string& out, const char* name, const char* type, const char* help) {
    out += "# HELP ";
    out += name;
    out += ' ';
    out += help;
    out += '\n';
    out += "# TYPE ";
    out += name;
    out += ' ';
    out += type;
    out += '\n';
}

void AppendSample(std::string& out, const char* name, const std::string& labels, uint64_t value) {
    out += name;
    if (!labels.empty()) {
        out += '{';
        out += labels;
        out += '}';
    }
    out += ' ';
    out += std::to_string(value);
    out += '\n';
}

void AppendHistogram(
    std::string& out,
    const char* name,
    const std::string& labels,
    const TPromHistogram& histogram)
{
    uint64_t cumulative = 0;
    for (size_t i = 0; i < TPromHistogram::kBoundCount; ++i) {
        cumulative += histogram.Bucket(i);
        out += name;
        out += "_bucket{";
        if (!labels.empty()) {
            out += labels;
            out += ',';
        }
        out += "le=\"";
        out += TPromHistogram::BoundLabel(i);
        out += "\"} ";
        out += std::to_string(cumulative);
        out += '\n';
    }
    cumulative += histogram.Overflow();
    out += name;
    out += "_bucket{";
    if (!labels.empty()) {
        out += labels;
        out += ',';
    }
    out += "le=\"+Inf\"} ";
    out += std::to_string(cumulative);
    out += '\n';

    out += name;
    out += "_sum{";
    out += labels;
    out += "} ";
    out += fmt::format("{:.6f}", static_cast<double>(histogram.SumMicros()) / 1000000.0);
    out += '\n';

    out += name;
    out += "_count{";
    out += labels;
    out += "} ";
    out += std::to_string(histogram.Count());
    out += '\n';
}

TPromSnapshot CollectSnapshot(
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats)
{
    TPromSnapshot snapshot;
    snapshot.Inflight = TransactionsInflight.load(std::memory_order_relaxed);
    for (const auto& stats : perThreadStats) {
        stats->CopyLiveMetrics(snapshot.Tx);
    }
    return snapshot;
}

std::string ReadRequest(int fd) {
    std::string data;
    char buf[512];
    while (data.size() < 4096 && data.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        data.append(buf, static_cast<size_t>(n));
    }
    return data;
}

bool IsMetricsRequest(const std::string& request) {
    if (request.size() < 5 || request.compare(0, 4, "GET ") != 0) {
        return false;
    }
    const size_t pathStart = 4;
    const size_t pathEnd = request.find(' ', pathStart);
    if (pathEnd == std::string::npos) {
        return false;
    }
    std::string path = request.substr(pathStart, pathEnd - pathStart);
    const size_t query = path.find('?');
    if (query != std::string::npos) {
        path.resize(query);
    }
    return path == "/metrics" || path == "/metrics/";
}

void WriteAll(int fd, const std::string& payload) {
    size_t off = 0;
    while (off < payload.size()) {
        const ssize_t n = ::send(fd, payload.data() + off, payload.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            return;
        }
        off += static_cast<size_t>(n);
    }
}

} // namespace

std::string RenderPrometheusMetrics(const TPromSnapshot& snapshot) {
    std::string out;
    out.reserve(8192);

    AppendHelp(
        out,
        "tpcc_transactions_total",
        "counter",
        "Transactions completed since the worker process started. result=success includes test-logic rollbacks.");
    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        const std::string typeLabel = std::string("type=\"") + PrometheusTypeLabel(i) + "\"";
        const auto& tx = snapshot.Tx[i];
        AppendSample(out, "tpcc_transactions_total", typeLabel + ",result=\"success\"", tx.Success);
        AppendSample(out, "tpcc_transactions_total", typeLabel + ",result=\"failure\"", tx.Failure);
    }

    AppendHelp(
        out,
        "tpcc_transaction_retries_total",
        "counter",
        "Retries caused by retryable errors since the worker process started.");
    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        const std::string typeLabel = std::string("type=\"") + PrometheusTypeLabel(i) + "\"";
        AppendSample(out, "tpcc_transaction_retries_total", typeLabel, snapshot.Tx[i].Retries);
    }

    AppendHelp(
        out,
        "tpcc_transaction_rollbacks_total",
        "counter",
        "Rollbacks required by the TPC-C transaction profile since the worker process started. Also counted as success.");
    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        const std::string typeLabel = std::string("type=\"") + PrometheusTypeLabel(i) + "\"";
        AppendSample(out, "tpcc_transaction_rollbacks_total", typeLabel, snapshot.Tx[i].Rollbacks);
    }

    AppendHelp(
        out,
        "tpcc_inflight",
        "gauge",
        "Transactions in flight at scrape time.");
    AppendSample(out, "tpcc_inflight", "", snapshot.Inflight);

    AppendHelp(
        out,
        "tpcc_transaction_duration_seconds",
        "histogram",
        "Cumulative client response time of transactions completed since the worker process started, in seconds.");
    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        const std::string typeLabel = std::string("type=\"") + PrometheusTypeLabel(i) + "\"";
        const auto& tx = snapshot.Tx[i];
        AppendHistogram(out, "tpcc_transaction_duration_seconds", typeLabel + ",result=\"success\"", tx.SuccessLatency);
        AppendHistogram(out, "tpcc_transaction_duration_seconds", typeLabel + ",result=\"failure\"", tx.FailureLatency);
    }

    AppendHelp(
        out,
        "tpcc_client_wait_seconds",
        "histogram",
        "Cumulative client-side wait of transactions completed since the worker process started, in seconds.");
    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        const std::string typeLabel = std::string("type=\"") + PrometheusTypeLabel(i) + "\"";
        const auto& tx = snapshot.Tx[i];
        AppendHistogram(out, "tpcc_client_wait_seconds", typeLabel + ",wait=\"admission\"", tx.AdmissionWait);
        AppendHistogram(out, "tpcc_client_wait_seconds", typeLabel + ",wait=\"session_pool\"", tx.SessionPoolWait);
        AppendHistogram(out, "tpcc_client_wait_seconds", typeLabel + ",wait=\"retry_backoff\"", tx.RetryBackoff);
    }
    return out;
}

TPrometheusExporter::TPrometheusExporter(
    int port,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats)
    : Stats_(&perThreadStats)
{
    if (port < 0 || port > 65535) {
        throw std::runtime_error("--metrics-port must be between 0 and 65535");
    }

    ListenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (ListenFd_ < 0) {
        throw std::runtime_error(std::string("prometheus listen socket: ") + std::strerror(errno));
    }
    int reuse = 1;
    // SO_REUSEADDR covers TIME_WAIT. It does not allow bind() when an
    // outbound connect() already owns this local port (EADDRINUSE). Callers
    // must construct the exporter before opening DBMS connection pools.
    ::setsockopt(ListenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(ListenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        const int err = errno;
        ::close(ListenFd_);
        ListenFd_ = -1;
        throw std::runtime_error(
            "prometheus bind port " + std::to_string(port) + ": " + std::strerror(err));
    }
    if (::listen(ListenFd_, 16) != 0) {
        const int err = errno;
        ::close(ListenFd_);
        ListenFd_ = -1;
        throw std::runtime_error(std::string("prometheus listen: ") + std::strerror(err));
    }

    sockaddr_in bound{};
    socklen_t boundLen = sizeof(bound);
    if (::getsockname(ListenFd_, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
        Port_ = ntohs(bound.sin_port);
    } else {
        Port_ = port;
    }

    Thread_ = std::thread([this] { Serve(); });
    LOG_I("Prometheus metrics listening on 0.0.0.0:" << Port_ << " path=/metrics");
}

TPrometheusExporter::~TPrometheusExporter() {
    Stop_.store(true, std::memory_order_release);
    if (ListenFd_ >= 0) {
        ::shutdown(ListenFd_, SHUT_RDWR);
        ::close(ListenFd_);
        ListenFd_ = -1;
    }
    if (Thread_.joinable()) {
        Thread_.join();
    }
}

void TPrometheusExporter::Serve() {
    while (!Stop_.load(std::memory_order_acquire)) {
        const int fd = ::accept(ListenFd_, nullptr, nullptr);
        if (fd < 0) {
            if (Stop_.load(std::memory_order_acquire) || errno == EBADF || errno == EINVAL) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        timeval tv{};
        tv.tv_sec = 2;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        const std::string request = ReadRequest(fd);
        std::string body;
        int status = 404;
        const char* statusText = "Not Found";
        if (IsMetricsRequest(request)) {
            status = 200;
            statusText = "OK";
            body = RenderPrometheusMetrics(CollectSnapshot(*Stats_));
        }
        std::string response = "HTTP/1.1 " + std::to_string(status) + " " + statusText + "\r\n";
        response += "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n";
        response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        response += "Connection: close\r\n\r\n";
        response += body;
        WriteAll(fd, response);
        ::close(fd);
    }
}

} // namespace NTpcc
