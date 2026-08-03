#pragma once

#include <adapter.h>

#include <string>

namespace NTpcc {

TCheckReport RunObChecks(const std::string& connectionString, const TCheckRequest& request);

void CheckSync(
    const std::string& connectionString,
    int warehouseCount,
    bool afterImport = false,
    const std::string& path = {});

class TObCheckAdapter final : public ICheckAdapter {
public:
    explicit TObCheckAdapter(std::string connectionString);

    TCheckReport Run(const TCheckRequest& request) override;

private:
    std::string ConnectionString_;
};

int RunCheckFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    bool afterImport,
    bool afterRun);

} // namespace NTpcc
