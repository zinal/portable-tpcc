#pragma once

#include "ydb_driver.h"

namespace NTpcc {

void CheckDbForInit(const TYdbConnectionConfig& connectionConfig) noexcept;
void CheckDbForImport(const TYdbConnectionConfig& connectionConfig) noexcept;
void CheckDbForIndexes(const TYdbConnectionConfig& connectionConfig) noexcept;
void CheckDbForRun(const TYdbConnectionConfig& connectionConfig, int expectedWhCount) noexcept;

} // namespace NTpcc
