#pragma once

#include "partition_config.h"

#include <string>

namespace NTpcc {

void InitSync(
    const std::string& connectionString,
    const std::string& path = {},
    const TPgPartitionConfig& partitionConfig = {});

void CreateIndexes(const std::string& connectionString, const std::string& path = {});

// Exposed for unit tests: build CREATE TABLE DDL for the given hash modulus
// (0 = unpartitioned tables). enableForeignKeys controls FOREIGN KEY clauses.
std::string BuildTpccSchemaDdl(int hashPartitionCount, bool enableForeignKeys = true);

} // namespace NTpcc
