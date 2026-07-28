#pragma once

#include <string>

namespace NTpcc {

void CheckSync(const std::string& connectionString, int warehouseCount, bool afterImport = false,
               const std::string& path = {});

} // namespace NTpcc
