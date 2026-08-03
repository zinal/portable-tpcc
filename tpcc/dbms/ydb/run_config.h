#pragma once

#include "ydb_driver.h"

#include <run_config_document.h>

#include <string>

namespace NTpcc {

TRunConfigDocument LoadRunConfigDocument(const std::string& path);
TYdbConnectionConfig BuildYdbConnectionConfig(const TRunConfigDocument& doc);

} // namespace NTpcc
