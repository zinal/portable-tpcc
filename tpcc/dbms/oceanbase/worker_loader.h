#pragma once

#include <optional>
#include <string>

namespace NTpcc {

int RunLoaderFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<int>& threadOverride = {});
int RunWorkerFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<std::string>& startAtRfc3339,
    const std::optional<int>& threadOverride = {},
    const std::optional<int>& maxInflightOverride = {});
int RunSchemaFromRunConfig(const std::string& runConfigPath, const std::string& instance);
int RunIndexesFromRunConfig(const std::string& runConfigPath, const std::string& instance);
int RunDropFromRunConfig(const std::string& runConfigPath, const std::string& instance);
int RunDebugFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    int repeats);
void DebugSync(
    const std::string& connectionString,
    const std::string& path,
    int warehouseCount,
    int repeats);

} // namespace NTpcc
