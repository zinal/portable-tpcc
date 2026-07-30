#pragma once

#include "types.h"

#include <string>

namespace NTpcc {

// Writes a machine-readable check report (atomic replace).
void WriteCheckReportJson(const std::string& path, const TCheckReport& report);

} // namespace NTpcc
