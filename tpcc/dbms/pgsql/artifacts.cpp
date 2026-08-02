#include "artifacts.h"

#include <nlohmann/json.hpp>

#include <constants.h>
#include <think_time.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace NTpcc {

namespace {

using Json = nlohmann::json;
using SysClock = std::chrono::system_clock;

const char* TransactionTypeToString(ETransactionType type) {
    switch (type) {
        case ETransactionType::NewOrder: return "new_order";
        case ETransactionType::Delivery: return "delivery";
        case ETransactionType::OrderStatus: return "order_status";
        case ETransactionType::Payment: return "payment";
        case ETransactionType::StockLevel: return "stock_level";
        default: return "unknown";
    }
}

Json WarehouseRangesJson(const std::vector<TWarehouseRange>& ranges) {
    Json arr = Json::array();
    for (const auto& r : ranges) {
        arr.push_back(Json::array({r.Start, r.End}));
    }
    return arr;
}

void WriteJsonAtomic(const std::string& path, const Json& value) {
    const std::string tmp = path + ".tmp";
    std::ofstream out(tmp);
    if (!out) {
        throw std::runtime_error("failed to write " + tmp);
    }
    out << value.dump(2);
    out.close();
    fs::rename(tmp, path);
}

std::string FormatTime(SysClock::time_point tp) {
    auto tt = SysClock::to_time_t(tp);
    std::ostringstream ss;
    ss << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

Json HistogramRaw(const THistogram& hist, const std::string& unit) {
    Json h = Json::object();
    h["layout"] = "linear_exp";
    h["unit"] = unit;
    h["hdr_till"] = hist.HdrTill();
    h["max_value"] = hist.MaxValue();
    h["total_count"] = hist.TotalCount();
    h["max_recorded"] = hist.MaxRecordedValue();
    h["buckets"] = hist.Buckets();
    return h;
}

Json HistogramPayload(const TTerminalStats::TTransactionStats& stats, const std::string& unit) {
    return HistogramRaw(stats.LatencyHistogramFullMs, unit);
}

} // anonymous

TArtifactPaths MakeArtifactPaths(const std::string& instanceDir) {
    TArtifactPaths p;
    p.InstanceDir = instanceDir;
    p.ProcessJson = instanceDir + "/process.json";
    p.ReadyJson = instanceDir + "/ready.json";
    p.ResultJson = instanceDir + "/result.json";
    p.ArtifactManifestJson = instanceDir + "/artifact-manifest.json";
    p.StdoutLog = instanceDir + "/stdout.log";
    p.StderrLog = instanceDir + "/stderr.log";
    return p;
}

void EnsureInstanceDir(const std::string& instanceDir) {
    fs::create_directories(instanceDir);
}

void WriteProcessJson(const TArtifactPaths& paths, const TRunConfigDocument& doc,
                      const std::string& instance, const std::string& role, int pid,
                      const std::string& instanceNonce) {
    Json j = {
        {"schema_version", 1},
        {"run_id", doc.RunId},
        {"instance", instance},
        {"role", role},
        {"pid", pid},
        {"run_config_sha256", doc.RunConfigSha256},
        {"instance_nonce", instanceNonce},
        {"started_at", FormatTime(SysClock::now())},
    };
    WriteJsonAtomic(paths.ProcessJson, j);
}

void WriteReadyJson(const TArtifactPaths& paths, const TRunConfigDocument& doc,
                    const std::string& instance, const std::vector<TWarehouseRange>& ranges,
                    const std::string& instanceNonce, const TClockCalibration& clockCalibration) {
    Json j = {
        {"schema_version", 1},
        {"run_id", doc.RunId},
        {"instance", instance},
        {"instance_nonce", instanceNonce},
        {"run_config_sha256", doc.RunConfigSha256},
        {"adapter", "pgsql"},
        {"warehouse_ranges", WarehouseRangesJson(ranges)},
        {"ready_at", FormatTime(SysClock::now())},
        {"clock_calibration", {
            {"measured_at", FormatTime(clockCalibration.MeasuredAt)},
            {"offset_ms", clockCalibration.OffsetMs},
            {"uncertainty_ms", clockCalibration.UncertaintyMs},
            {"rtt_ms", clockCalibration.RttMs},
            {"time_source", clockCalibration.TimeSource},
        }},
    };
    WriteJsonAtomic(paths.ReadyJson, j);
}

void WriteLoaderResultJson(const TArtifactPaths& paths, const TRunConfigDocument& doc,
                           const std::string& instance, const TLoaderAssignment& assign, int exitCode) {
    Json j = {
        {"schema_version", 1},
        {"run_id", doc.RunId},
        {"instance", instance},
        {"role", "loader"},
        {"run_config_sha256", doc.RunConfigSha256},
        {"assignment", {
            {"instance", assign.Instance},
            {"host", assign.Host},
            {"warehouse_ranges", WarehouseRangesJson(assign.WarehouseRanges)},
            {"owns_global_data", assign.OwnsGlobalData},
        }},
        {"exit_status", exitCode},
        {"completed_at", FormatTime(SysClock::now())},
    };
    WriteJsonAtomic(paths.ResultJson, j);
}

void WriteWorkerResultJson(const TArtifactPaths& paths, const TRunConfigDocument& doc,
                           const std::string& instance, const TWorkerAssignment& assign,
                           const TTerminalStats& stats, bool highResHistogram,
                           SysClock::time_point rampStart,
                           SysClock::time_point measurementStart,
                           SysClock::time_point measurementEnd,
                           SysClock::time_point drainDeadline,
                           double measureSeconds, int exitCode,
                           const std::string& instanceNonce) {
    Json counters = Json::object();
    Json histograms = Json::object();
    size_t totalFailed = 0;
    size_t totalRetried = 0;
    size_t totalNewOrderOk = 0;
    const std::string histUnit = doc.Histogram.Configured ? doc.Histogram.Unit : "ms";

    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        const auto type = static_cast<ETransactionType>(i);
        const auto& s = stats.GetStats(type);
        const auto ok = s.OK.load(std::memory_order_relaxed);
        const auto failed = s.Failed.load(std::memory_order_relaxed);
        const auto retried = s.Retried.load(std::memory_order_relaxed);
        totalFailed += failed;
        totalRetried += retried;
        if (type == ETransactionType::NewOrder) {
            totalNewOrderOk = ok;
        }
        if (ok > 0 || failed > 0 || retried > 0 || s.LatencyHistogramFullMs.TotalCount() > 0) {
            counters[std::string(TransactionTypeToString(type)) + "_ok"] = ok;
            counters[std::string(TransactionTypeToString(type)) + "_failed"] = failed;
            counters[std::string(TransactionTypeToString(type)) + "_retried"] = retried;
            histograms[TransactionTypeToString(type)] = HistogramPayload(s, histUnit);
        }
    }

    const double tpmc = measureSeconds > 0 ? (totalNewOrderOk / measureSeconds * 60.0) : 0.0;
    const size_t whCount = CountWarehouses(assign.WarehouseRanges);

    Json mix = Json::object();
    Json keying = Json::object();
    Json think = Json::object();
    const char* keys[] = {"new_order", "delivery", "order_status", "payment", "stock_level"};
    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        mix[keys[i]] = doc.Workload.PerTx[i].Weight;
        keying[keys[i]] = doc.Workload.PerTx[i].KeyingTimeMs;
        think[keys[i]] = doc.Workload.PerTx[i].ThinkTimeMs;
    }

    Json j = {
        {"schema_version", 1},
        {"run_id", doc.RunId},
        {"instance", instance},
        {"instance_nonce", instanceNonce},
        {"role", "worker"},
        {"run_config_sha256", doc.RunConfigSha256},
        {"assignment", {
            {"instance", assign.Instance},
            {"host", assign.Host},
            {"warehouse_ranges", WarehouseRangesJson(assign.WarehouseRanges)},
            {"threads", assign.Threads},
            {"max_inflight", assign.MaxInflight},
        }},
        {"phases", {
            {"ramp_start", FormatTime(rampStart)},
            {"measurement_start", FormatTime(measurementStart)},
            {"measurement_end", FormatTime(measurementEnd)},
            {"drain_deadline", FormatTime(drainDeadline)},
        }},
        {"settings", {
            {"workload", {
                {"terminals_per_warehouse", doc.Workload.TerminalsPerWarehouse},
                {"transaction_mix", mix},
                {"keying_time_ms", keying},
                {"think_time_ms", think},
                {"pacing", doc.PacingEnabled ? "enabled" : "disabled"},
                {"think_time_distribution", ThinkTimeDistributionToString(doc.ThinkTimeDistribution)},
            }},
            {"histogram", {
                {"unit", histUnit},
                {"lowest", doc.Histogram.Configured ? doc.Histogram.Lowest : 1},
                {"highest", doc.Histogram.Configured ? doc.Histogram.Highest : 32768},
                {"significant_figures", doc.Histogram.Configured ? doc.Histogram.SignificantFigures : 3},
                {"layout", "linear_exp"},
                {"hdr_till", stats.HdrTill()},
                {"max_value", stats.MaxValue()},
            }},
        }},
        {"counters", counters},
        {"histograms", histograms},
        {"metrics", {
            {"new_order_tpmc", tpmc},
            {"measurement_seconds", measureSeconds},
            {"warehouses", whCount},
            {"total_failed", totalFailed},
            {"total_retried", totalRetried},
            {"high_res_histogram", highResHistogram},
        }},
        {"versions", {
            {"adapter", "pgsql"},
            {"binary", doc.Binary.empty() ? "tpcc-pgsql" : doc.Binary},
        }},
        {"exit_status", exitCode},
        {"completed_at", FormatTime(SysClock::now())},
    };
    WriteJsonAtomic(paths.ResultJson, j);
}

void WriteArtifactManifest(const TArtifactPaths& paths, const std::string& instance,
                           const std::string& instanceNonce, int exitCode) {
    Json payloads = Json::array();
    auto addPayload = [&](const std::string& rel) {
        const std::string full = paths.InstanceDir + "/" + rel;
        if (!fs::exists(full)) {
            return;
        }
        payloads.push_back({
            {"path", rel},
            {"size", static_cast<int64_t>(fs::file_size(full))},
            {"sha256", ComputeFileSha256Hex(full)},
        });
    };
    addPayload("result.json");
    addPayload("ready.json");
    addPayload("process.json");
    addPayload("stdout.log");
    addPayload("stderr.log");

    Json manifest = {
        {"schema_version", 1},
        {"instance", instance},
        {"instance_nonce", instanceNonce},
        {"finalized", true},
        {"exit_status", exitCode},
        {"payloads", payloads},
    };
    WriteJsonAtomic(paths.ArtifactManifestJson, manifest);
}

std::string GenerateInstanceNonce() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << std::hex << dist(gen) << dist(gen);
    return ss.str();
}

} // namespace NTpcc
