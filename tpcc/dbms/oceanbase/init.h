#pragma once

#include "schema_options.h"

#include <string>

namespace NTpcc {

void InitSync(
    const std::string& connectionString,
    const std::string& path = {},
    const TObSchemaOptions& options = {});

void CreateIndexes(
    const std::string& connectionString,
    const std::string& path = {},
    bool useLocalIndexes = false,
    int indexParallel = OB_DEFAULT_INDEX_PARALLEL);

void AnalyzeTables(const std::string& connectionString, const std::string& path = {});

} // namespace NTpcc
