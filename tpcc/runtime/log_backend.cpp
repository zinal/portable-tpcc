#include <log_backend.h>

#include <library/cpp/logger/backend_creator.h>

#include <util/stream/output.h>
#include <util/system/guard.h>

#include <utility>

namespace {
constexpr size_t DefaultCaptureLines = 1000;
}

namespace NTpcc {

namespace {

std::shared_ptr<TLog> GlobalLog;
TLogBackendWithCapture* GlobalLogBackend = nullptr;

} // namespace

TLogBackendWithCapture::TLogBackendWithCapture(const TString& type, ELogPriority priority, size_t maxLines)
    : RealBackend(CreateLogBackend(type, priority, true).Release())
    , MaxLines(maxLines)
{
    CapturedLines.reserve(MaxLines * 2);
}

void TLogBackendWithCapture::StartCapture() {
    TGuard guard(CapturingMutex);
    IsCapturing = true;
}

void TLogBackendWithCapture::StopCapture() {
    TGuard guard(CapturingMutex);

    if (!IsCapturing) {
        return;
    }

    IsCapturing = false;

    ProcessNewLines(true);
    CapturedLines.clear();
    LogLines.clear();
    TruncatedCount = 0;
}

void TLogBackendWithCapture::StopCaptureAndFlush(IOutputStream& os) {
    TGuard guard(CapturingMutex);

    if (!IsCapturing) {
        return;
    }

    IsCapturing = false;

    ProcessNewLines(true);

    for (const auto& [priority, line] : LogLines) {
        Y_UNUSED(priority);
        os << line;
    }

    CapturedLines.clear();
    LogLines.clear();
    TruncatedCount = 0;
}

void TLogBackendWithCapture::GetLogLines(const TLogProcessor& processor) {
    ProcessNewLines(false);

    if (TruncatedCount > 0) {
        processor(TLOG_INFO, "... logs truncated: " + std::to_string(TruncatedCount) + " lines");
    }

    for (const auto& [priority, line] : LogLines) {
        processor(priority, line);
    }
}

void TLogBackendWithCapture::ProcessNewLines(bool lockTaken) {
    std::vector<std::pair<ELogPriority, std::string>> newLines;
    newLines.reserve(MaxLines * 2);
    if (lockTaken) {
        newLines.swap(CapturedLines);
    } else {
        TGuard guard(CapturingMutex);
        newLines.swap(CapturedLines);
    }

    if (newLines.empty()) {
        return;
    }

    auto currentSize = LogLines.size();
    auto newSize = currentSize + newLines.size();
    if (newSize > MaxLines && newLines.size() > MaxLines) {
        TruncatedCount += LogLines.size();
        LogLines.clear();

        size_t newLinesTruncateCount = newLines.size() - MaxLines;
        TruncatedCount += newLinesTruncateCount;
        for (size_t i = newLinesTruncateCount; i < newLines.size(); ++i) {
            LogLines.emplace_back(std::move(newLines[i]));
        }
    } else {
        size_t popCount = 0;
        if (newSize > MaxLines) {
            popCount = newSize - MaxLines;
        }
        TruncatedCount += popCount;
        for (size_t i = 0; i < popCount && !LogLines.empty(); ++i) {
            LogLines.pop_front();
        }
        for (auto& line : newLines) {
            LogLines.emplace_back(std::move(line));
        }
    }
}

void TLogBackendWithCapture::WriteData(const TLogRecord& record) {
    {
        TGuard guard(CapturingMutex);
        if (IsCapturing) {
            CapturedLines.emplace_back(record.Priority, std::string(record.Data, record.Len));
            return;
        }
    }
    RealBackend->WriteData(record);
}

void InitLogging(ELogPriority level) {
    // TLog takes ownership of the backend via THolder.
    GlobalLogBackend = new TLogBackendWithCapture("cerr", level, DefaultCaptureLines);
    GlobalLog = std::make_shared<TLog>(THolder<TLogBackend>(GlobalLogBackend));
}

std::shared_ptr<TLog>& GetLog() {
    if (!GlobalLog) {
        InitLogging();
    }
    return GlobalLog;
}

TLogBackendWithCapture* GetLogBackend() {
    GetLog();
    return GlobalLogBackend;
}

void StartLogCapture() {
    if (auto* backend = GetLogBackend()) {
        backend->StartCapture();
    }
}

void StopLogCapture() {
    if (GlobalLogBackend) {
        GlobalLogBackend->StopCapture();
    }
}

void StopLogCaptureAndFlush(IOutputStream& os) {
    if (GlobalLogBackend) {
        GlobalLogBackend->StopCaptureAndFlush(os);
    }
}

} // namespace NTpcc
