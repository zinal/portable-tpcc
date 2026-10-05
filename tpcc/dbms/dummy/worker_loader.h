#pragma once

#include "dummy_delay.h"

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
    const std::optional<int>& maxInflightOverride = {},
    const std::optional<int>& metricsPort = {});
int RunSchemaFromRunConfig(const std::string& runConfigPath, const std::string& instance);
int RunIndexesFromRunConfig(const std::string& runConfigPath, const std::string& instance);
int RunDropFromRunConfig(const std::string& runConfigPath, const std::string& instance);
int RunDebugFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    int repeats);
void DebugSync(const TDummyDelayConfig& delay, int warehouseCount, int repeats);

} // namespace NTpcc
