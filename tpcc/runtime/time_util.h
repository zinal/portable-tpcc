#pragma once

#include <chrono>
#include <string>

namespace NTpcc {

// Parse RFC 3339 UTC timestamps such as "2026-07-28T12:00:15Z".
// Fractional seconds are accepted and truncated to whole seconds.
std::chrono::system_clock::time_point ParseRfc3339Utc(const std::string& text);

std::string FormatRfc3339Utc(std::chrono::system_clock::time_point tp);

} // namespace NTpcc
