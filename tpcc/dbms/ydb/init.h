#pragma once

#include "ydb_driver.h"

#include <string>

namespace NTpcc {

void InitSync(const TYdbConnectionConfig& connectionConfig, int warehouseCount);
void CreateIndexes(const TYdbConnectionConfig& connectionConfig);

} // namespace NTpcc
