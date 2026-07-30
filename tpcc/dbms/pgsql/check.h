#pragma once

#include <adapter.h>

#include <string>

namespace NTpcc {

// Runs the shared check catalog against PostgreSQL and returns a structured report.
// Does not throw on individual check failures; sets FailedCount / ErrorCount instead.
TCheckReport RunPgChecks(const std::string& connectionString, const TCheckRequest& request);

// Standalone CLI helper: prints progress and throws if any check fails.
void CheckSync(const std::string& connectionString, int warehouseCount, bool afterImport = false,
               const std::string& path = {});

class TPgCheckAdapter final : public ICheckAdapter {
public:
    explicit TPgCheckAdapter(std::string connectionString);

    TCheckReport Run(const TCheckRequest& request) override;

private:
    std::string ConnectionString_;
};

// Orchestrated check role: writes results/<run>/checks/<phase>.json
int RunCheckFromRunConfig(const std::string& runConfigPath, const std::string& instance,
                          bool afterImport, bool afterRun);

} // namespace NTpcc
