#include "report.h"

#include "catalog.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace fs = std::filesystem;

namespace NTpcc {

void RecordCheckResult(TCheckReport& report, TCheckResult result, bool print) {
    switch (result.Status) {
        case ECheckStatus::Passed: ++report.PassedCount; break;
        case ECheckStatus::Failed: ++report.FailedCount; break;
        case ECheckStatus::Skipped: ++report.SkippedCount; break;
        case ECheckStatus::Error: ++report.ErrorCount; break;
    }
    if (print) {
        const char* tag = "[OK]";
        if (result.Status == ECheckStatus::Failed || result.Status == ECheckStatus::Error) {
            tag = "[Failed]";
        } else if (result.Status == ECheckStatus::Skipped) {
            tag = "[Skipped]";
        }
        const int index = report.PassedCount + report.FailedCount
            + report.SkippedCount + report.ErrorCount;
        std::cout << "Checking ";
        if (report.ProgressTotal > 0) {
            std::cout << "[" << index << "/" << report.ProgressTotal << "] ";
        }
        std::cout << result.Title << " " << tag;
        if (!result.Detail.empty() && result.Status != ECheckStatus::Passed) {
            std::cout << ": " << result.Detail;
        }
        std::cout << std::endl;
    }
    report.Results.push_back(std::move(result));
}

void RecordCheckResult(
    TCheckReport& report,
    std::string id,
    ECheckStatus status,
    const std::string& detail,
    bool print)
{
    TCheckResult result;
    result.Id = std::move(id);
    if (const auto* entry = FindCheckCatalogEntry(result.Id)) {
        result.Title = std::string(entry->Title);
    } else {
        result.Title = result.Id;
    }
    result.Status = status;
    result.Detail = detail;
    RecordCheckResult(report, std::move(result), print);
}

void WriteCheckReportJson(const std::string& path, const TCheckReport& report) {
    nlohmann::json checks = nlohmann::json::array();
    for (const auto& r : report.Results) {
        checks.push_back({
            {"id", r.Id},
            {"title", r.Title},
            {"status", CheckStatusToString(r.Status)},
            {"detail", r.Detail},
        });
    }

    nlohmann::json j = {
        {"schema_version", 1},
        {"run_id", report.RunId},
        {"instance", report.Instance},
        {"phase", report.Phase},
        {"warehouse_count", report.WarehouseCount},
        {"passed", report.PassedCount},
        {"failed", report.FailedCount},
        {"skipped", report.SkippedCount},
        {"errors", report.ErrorCount},
        {"ok", report.Ok()},
        {"checks", std::move(checks)},
    };

    fs::create_directories(fs::path(path).parent_path());
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) {
            throw std::runtime_error("failed to write " + tmp);
        }
        out << j.dump(2);
    }
    fs::rename(tmp, path);
}

} // namespace NTpcc
