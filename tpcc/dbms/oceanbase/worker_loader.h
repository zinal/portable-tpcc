#pragma once

#include <optional>
#include <string>

namespace NTpcc {

int RunLoaderFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<int>& threadOverride = {},
    const std::optional<int>& queryTimeoutSeconds = {});
int RunWorkerFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<std::string>& startAtRfc3339,
    const std::optional<int>& threadOverride = {},
    const std::optional<int>& maxInflightOverride = {},
    const std::optional<int>& metricsPort = {});
int RunSchemaFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<int>& queryTimeoutSeconds = {});
int RunIndexesFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<int>& queryTimeoutSeconds = {});
int RunDropFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<int>& queryTimeoutSeconds = {});
int RunDebugFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    int repeats,
    const std::optional<int>& queryTimeoutSeconds = {});
void DebugSync(
    const std::string& connectionString,
    const std::string& path,
    int warehouseCount,
    int repeats);

} // namespace NTpcc
