#pragma once

#include "ydb_driver.h"

#include <adapter.h>

namespace NTpcc {

// Evaluates the shared catalog and prints Checking [i/n] … [OK]/[Failed]/[Skipped]
// after each catalog id (RecordCheckResult; specification §9.2).
TCheckReport RunYdbChecks(const TYdbConnectionConfig& connectionConfig, const TCheckRequest& request);

// Standalone CLI helper: prints progress and throws if any check fails.
void CheckSync(
    const TYdbConnectionConfig& connectionConfig,
    int warehouseCount,
    bool afterImport = false,
    int checkConcurrency = 1);

class TYdbCheckAdapter final : public ICheckAdapter {
public:
    explicit TYdbCheckAdapter(TYdbConnectionConfig connectionConfig);

    TCheckReport Run(const TCheckRequest& request) override;

private:
    TYdbConnectionConfig ConnectionConfig_;
};

int RunCheckFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    bool afterImport,
    bool afterRun,
    int checkConcurrency = 1);

} // namespace NTpcc
