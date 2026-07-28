#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/fmt/fmt.h>

namespace NTpcc {

// Initialize the global logger. Call once at startup.
void InitLogging(spdlog::level::level_enum level = spdlog::level::info);

// Get the shared logger instance
std::shared_ptr<spdlog::logger>& GetLogger();

} // namespace NTpcc

#define LOG_T(...) SPDLOG_LOGGER_TRACE(NTpcc::GetLogger(), __VA_ARGS__)
#define LOG_D(...) SPDLOG_LOGGER_DEBUG(NTpcc::GetLogger(), __VA_ARGS__)
#define LOG_I(...) SPDLOG_LOGGER_INFO(NTpcc::GetLogger(), __VA_ARGS__)
#define LOG_W(...) SPDLOG_LOGGER_WARN(NTpcc::GetLogger(), __VA_ARGS__)
#define LOG_E(...) SPDLOG_LOGGER_ERROR(NTpcc::GetLogger(), __VA_ARGS__)
