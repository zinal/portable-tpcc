#pragma once

#include <log.h>

#include <library/cpp/logger/backend.h>
#include <library/cpp/logger/log.h>

#include <util/generic/string.h>
#include <util/system/mutex.h>

#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace NTpcc {

// Either writes logs to the real backend or captures them for TUI display.
class TLogBackendWithCapture : public TLogBackend {
public:
    explicit TLogBackendWithCapture(const TString& type, ELogPriority priority, size_t maxLines);
    ~TLogBackendWithCapture() override = default;

    void StartCapture();
    void StopCapture();
    void StopCaptureAndFlush(IOutputStream& os);

    // Get current log lines to display in TUI.
    // Assumes single consumer, multiple producers.
    void GetLogLines(const TLogProcessor& processor);

    void WriteData(const TLogRecord& rec) override;

    void ReopenLog() override {
        RealBackend->ReopenLog();
    }

    ELogPriority FiltrationLevel() const override {
        return RealBackend->FiltrationLevel();
    }

private:
    void ProcessNewLines(bool logTaken);

private:
    THolder<TLogBackend> RealBackend;
    const size_t MaxLines;

    std::deque<std::pair<ELogPriority, std::string>> LogLines;
    size_t TruncatedCount = 0;

    TMutex CapturingMutex;
    bool IsCapturing = false;
    std::vector<std::pair<ELogPriority, std::string>> CapturedLines;
};

// Global capture backend installed by InitLogging (owned by GetLog()).
TLogBackendWithCapture* GetLogBackend();

void StartLogCapture();
void StopLogCapture();
void StopLogCaptureAndFlush(IOutputStream& os);

} // namespace NTpcc
