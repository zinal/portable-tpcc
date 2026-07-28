#pragma once

#include <log.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace NTpcc {

class TLogCapture;

using TLogProcessor = std::function<void(spdlog::level::level_enum, const std::string&)>;

void StartLogCapture(TLogCapture& capture);
void StopLogCapture();

// Captures recent log lines for TUI display
class TLogCapture {
public:
    explicit TLogCapture(size_t maxLines = 1000);

    void AddLine(const std::string& line);
    std::vector<std::string> GetLines() const;
    void Clear();

private:
    mutable std::mutex Mutex_;
    std::vector<std::string> Lines_;
    size_t MaxLines_;
    size_t WritePos_ = 0;
    bool Wrapped_ = false;
};

} // namespace NTpcc
