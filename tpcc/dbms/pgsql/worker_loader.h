#pragma once

#include <string>

namespace NTpcc {

int RunLoaderFromRunConfig(const std::string& runConfigPath, const std::string& instance);
int RunWorkerFromRunConfig(const std::string& runConfigPath, const std::string& instance);

} // namespace NTpcc
