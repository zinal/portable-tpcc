#pragma once

#include <prom_histogram.h>
#include <constants.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace NTpcc {

class TTerminalStats;

// One scrape of process-lifetime counters and histograms, plus inflight
// sampled at scrape time. Counters and histogram buckets only increase
// until the process exits.
struct TPromSnapshot {
    uint64_t Inflight = 0;
    std::array<TPromTxSnapshot, TRANSACTION_TYPE_COUNT> Tx{};
};

std::string RenderPrometheusMetrics(const TPromSnapshot& snapshot);

// HTTP server on 0.0.0.0:port serving GET /metrics in Prometheus text format.
// Each scrape copies `perThreadStats` (cumulative since process start) and
// the current inflight gauge. `perThreadStats` must outlive this object and
// must not be resized. port 0 asks the kernel for an ephemeral port (tests).
// Listen failures throw.
class TPrometheusExporter {
public:
    TPrometheusExporter(
        int port,
        const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats);
    ~TPrometheusExporter();

    TPrometheusExporter(const TPrometheusExporter&) = delete;
    TPrometheusExporter& operator=(const TPrometheusExporter&) = delete;

    int Port() const { return Port_; }

private:
    void Serve();

    int Port_ = 0;
    int ListenFd_ = -1;
    std::atomic<bool> Stop_{false};
    const std::vector<std::shared_ptr<TTerminalStats>>* Stats_ = nullptr;
    std::thread Thread_;
};

} // namespace NTpcc
