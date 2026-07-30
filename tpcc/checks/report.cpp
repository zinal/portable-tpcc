#include "report.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace NTpcc {

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
