#pragma once

#include <string>

namespace NTpcc {

void CheckDbForInit(const std::string& connectionString, const std::string& path = {}) noexcept;
void CheckDbForImport(const std::string& connectionString, const std::string& path = {}) noexcept;
void CheckDbForRun(
    const std::string& connectionString,
    int expectedWhCount,
    const std::string& path = {}) noexcept;

} // namespace NTpcc
