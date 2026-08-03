#pragma once

#include <run_config_document.h>

#include <string>

namespace NTpcc {

TRunConfigDocument LoadRunConfigDocument(const std::string& path);
std::string BuildPgConnectionString(const TRunConfigDocument& doc);

} // namespace NTpcc
