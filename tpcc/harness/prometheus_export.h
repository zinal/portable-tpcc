#pragma once

#include <prom_histogram.h>
#include <constants.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace NTpcc {

class TTerminalStats;

// Values published for one stats-interval window. Counts and histograms are
// the samples that completed during that window, not process lifetime totals.
// Success includes test-logic rollbacks. tpmC is New-Order successes in the
// window, per minute. Inflight is the value sampled when the window closed.
struct TPromIntervalSnapshot {
    double IntervalSeconds = 0;
    double Tpmc = 0;
    uint64_t Inflight = 0;
    std::array<TPromTxSnapshot, TRANSACTION_TYPE_COUNT> Tx{};
};

std::string RenderPrometheusMetrics(const TPromIntervalSnapshot& snapshot);

// HTTP server on 0.0.0.0:port serving GET /metrics in Prometheus text format.
// port 0 asks the kernel for an ephemeral port (tests). Listen failures throw.
class TPrometheusExporter {
public:
    explicit TPrometheusExporter(int port);
    ~TPrometheusExporter();

    TPrometheusExporter(const TPrometheusExporter&) = delete;
    TPrometheusExporter& operator=(const TPrometheusExporter&) = delete;

    void Publish(std::string body);
    int Port() const { return Port_; }

private:
    void Serve();

    int Port_ = 0;
    int ListenFd_ = -1;
    std::atomic<bool> Stop_{false};
    std::mutex Mu_;
    std::string Body_;
    std::thread Thread_;
};

struct TProgressDisplayState;

// Called on each console stats tick. The first tick only arms the window.
// Later ticks publish the samples collected since the previous tick.
// exporter == nullptr is a no-op.
void NotePrometheusInterval(
    TProgressDisplayState& state,
    std::chrono::steady_clock::time_point now,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    TPrometheusExporter* exporter);

// Publish whatever remains in the open window (end of the run).
void FlushPrometheusInterval(
    TProgressDisplayState& state,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    TPrometheusExporter* exporter);

} // namespace NTpcc
