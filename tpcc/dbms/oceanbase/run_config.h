#pragma once

#include <run_config_document.h>

#include <optional>
#include <string>

namespace NTpcc {

TRunConfigDocument LoadRunConfigDocument(const std::string& path);
std::string BuildObConnectionString(const TRunConfigDocument& doc);

// Launch-time --query-timeout. When set, replaces database.options.query_timeout
// for this process. Does not rewrite run-config.json. N must be > 0 (seconds).
void ApplyObQueryTimeoutOverride(TRunConfigDocument& doc, const std::optional<int>& seconds);

} // namespace NTpcc
