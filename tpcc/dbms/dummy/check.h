#pragma once

#include <adapter.h>

#include <string>

namespace NTpcc {

TCheckReport RunDummyChecks(const TCheckRequest& request);

void CheckSync(int warehouseCount, bool afterImport = false, int checkConcurrency = 1);

class TDummyCheckAdapter final : public ICheckAdapter {
public:
    TCheckReport Run(const TCheckRequest& request) override;
};

int RunCheckFromRunConfig(const std::string& runConfigPath, const std::string& instance,
                          bool afterImport, bool afterRun, int checkConcurrency = 1);

} // namespace NTpcc
