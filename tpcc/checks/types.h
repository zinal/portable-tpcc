#pragma once

#include <string>
#include <vector>

namespace NTpcc {

enum class ECheckStatus {
    Passed,
    Failed,
    Skipped,
    Error,
};

struct TCheckResult {
    std::string Id;
    std::string Title;
    ECheckStatus Status = ECheckStatus::Error;
    std::string Detail;
};

struct TCheckReport {
    std::string RunId;
    std::string Instance;
    std::string Phase; // "after-import" | "after-test"
    int WarehouseCount = 0;
    std::vector<TCheckResult> Results;
    int FailedCount = 0;
    int PassedCount = 0;
    int SkippedCount = 0;
    int ErrorCount = 0;

    bool Ok() const {
        return FailedCount == 0 && ErrorCount == 0;
    }
};

inline const char* CheckStatusToString(ECheckStatus status) {
    switch (status) {
        case ECheckStatus::Passed: return "passed";
        case ECheckStatus::Failed: return "failed";
        case ECheckStatus::Skipped: return "skipped";
        case ECheckStatus::Error: return "error";
    }
    return "error";
}

} // namespace NTpcc
