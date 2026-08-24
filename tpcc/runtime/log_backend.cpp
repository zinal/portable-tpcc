#include <log_backend.h>

#include <library/cpp/logger/backend_creator.h>

#include <util/stream/output.h>
#include <util/system/guard.h>
#include <util/system/mutex.h>

#include <cstdio>
#include <unistd.h>
#include <utility>

namespace {
constexpr size_t DefaultCaptureLines = 1000;
}

namespace NTpcc {

namespace {

std::shared_ptr<TLog> GlobalLog;
TLogBackendWithCapture* GlobalLogBackend = nullptr;
TMutex LogMutex;

void EnsureDefaultLoggingLocked() {
    if (GlobalLog) {
        return;
    }
    GlobalLogBackend = new TLogBackendWithCapture("cerr", TLOG_INFO, DefaultCaptureLines);
    GlobalLog = std::make_shared<TLog>(THolder<TLogBackend>(GlobalLogBackend));
}

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
    // Create the threaded backend outside the lock: its constructor starts
    // a worker thread. Publish the pointers atomically w.r.t. GetLog().
    auto* backend = new TLogBackendWithCapture("cerr", level, DefaultCaptureLines);
    auto log = std::make_shared<TLog>(THolder<TLogBackend>(backend));
    TGuard guard(LogMutex);
    GlobalLogBackend = backend;
    GlobalLog = std::move(log);
}

std::shared_ptr<TLog> GetLog() {
    TGuard guard(LogMutex);
    EnsureDefaultLoggingLocked();
    return GlobalLog;
}

TLogBackendWithCapture* GetLogBackend() {
    TGuard guard(LogMutex);
    EnsureDefaultLoggingLocked();
    return GlobalLogBackend;
}

void StartLogCapture() {
    if (auto* backend = GetLogBackend()) {
        backend->StartCapture();
    }
}

void StopLogCapture() {
    TGuard guard(LogMutex);
    if (GlobalLogBackend) {
        GlobalLogBackend->StopCapture();
    }
}

void StopLogCaptureAndFlush(IOutputStream& os) {
    TGuard guard(LogMutex);
    if (GlobalLogBackend) {
        GlobalLogBackend->StopCaptureAndFlush(os);
    }
}

void FlushLogs() {
    std::shared_ptr<TLog> log;
    {
        TGuard guard(LogMutex);
        log = GlobalLog;
    }
    if (log) {
        // ReopenLog on TOwningThreadedLogBackend waits until previously queued
        // records are written to the slave (Cerr).
        log->ReopenLog();
    }
    Cerr.Flush();
    Cout.Flush();
    fflush(stderr);
    fflush(stdout);
    // Best-effort: ignore EINVAL when stdio is not a regular file.
    ::fsync(STDOUT_FILENO);
    ::fsync(STDERR_FILENO);
}

} // namespace NTpcc
