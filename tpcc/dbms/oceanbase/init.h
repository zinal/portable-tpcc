#pragma once

#include "schema_options.h"

#include <string>
#include <vector>

namespace NTpcc {

// Resolved physical layout for OceanBase schema DDL.
struct TObSchemaLayout {
    bool UseClusterLayout = false;
    // Replicate DB-wide item to every observer (DUPLICATE_SCOPE = cluster).
    bool DuplicateItem = false;
    int PartitionCount = 1;
};

std::vector<std::string> BuildObCreateStatements(
    const TObSchemaLayout& layout,
    const TObSchemaOptions& options);

void InitSync(
    const std::string& connectionString,
    const std::string& path = {},
    const TObSchemaOptions& options = {});

void CreateIndexes(
    const std::string& connectionString,
    const std::string& path = {},
    bool useLocalIndexes = false,
    int indexParallel = OB_DEFAULT_INDEX_PARALLEL);

void AnalyzeTables(
    const std::string& connectionString,
    const std::string& path = {},
    const TObSchemaOptions& options = {});

} // namespace NTpcc
