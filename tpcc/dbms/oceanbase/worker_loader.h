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
    const std::optional<int>& threadOverride = {});
int RunSchemaFromRunConfig(const std::string& runConfigPath, const std::string& instance);
int RunIndexesFromRunConfig(const std::string& runConfigPath, const std::string& instance);
int RunCleanFromRunConfig(const std::string& runConfigPath, const std::string& instance);

} // namespace NTpcc
