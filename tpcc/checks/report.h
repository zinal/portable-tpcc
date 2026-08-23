#pragma once

#include "types.h"

#include <string>

namespace NTpcc {

// Appends one catalog-id result and, when `print` is true, emits the
// recommended stdout progress line:
// `Checking [i/n] <title> [OK]/[Failed]/[Skipped]` when ProgressTotal > 0,
// otherwise `Checking <title> [OK]/[Failed]/[Skipped]`.
// Call this as soon as that catalog id finishes (all warehouse chunks).
// See specification §9.2.
void RecordCheckResult(TCheckReport& report, TCheckResult result, bool print = true);
void RecordCheckResult(
    TCheckReport& report,
    std::string id,
    ECheckStatus status,
    const std::string& detail = {},
    bool print = true);

// Writes a machine-readable check report (atomic replace).
void WriteCheckReportJson(const std::string& path, const TCheckReport& report);

} // namespace NTpcc
