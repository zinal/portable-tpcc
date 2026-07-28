#pragma once

#include <string>

namespace NTpcc {

void InitSync(const std::string& connectionString, const std::string& path = {});
void CreateIndexes(const std::string& connectionString, const std::string& path = {});

} // namespace NTpcc
