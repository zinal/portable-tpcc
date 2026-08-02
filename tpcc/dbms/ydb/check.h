#pragma once

#include "ydb_driver.h"

#include <adapter.h>

namespace NTpcc {

TCheckReport RunYdbChecks(const TYdbConnectionConfig& connectionConfig, const TCheckRequest& request);

void CheckSync(const TYdbConnectionConfig& connectionConfig, int warehouseCount, bool afterImport = false);

class TYdbCheckAdapter final : public ICheckAdapter {
public:
    explicit TYdbCheckAdapter(TYdbConnectionConfig connectionConfig);

    TCheckReport Run(const TCheckRequest& request) override;

private:
    TYdbConnectionConfig ConnectionConfig_;
};

int RunCheckFromRunConfig(const std::string& runConfigPath, const std::string& instance,
                          bool afterImport, bool afterRun);

} // namespace NTpcc
