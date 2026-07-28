#pragma once

#include <atomic>
#include <cstddef>
#include <stop_token>
#include <string>

namespace NTpcc {

std::string GetFormattedSize(size_t size);

std::string GetEffectiveSchema(const std::string& path);

std::stop_source& GetGlobalInterruptSource();
std::atomic<bool>& GetGlobalErrorVariable();

inline void RequestStopWithError() {
    GetGlobalErrorVariable().store(true);
    GetGlobalInterruptSource().request_stop();
}

size_t NumberOfMyCpus();

} // namespace NTpcc
