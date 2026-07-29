#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace NTpcc {

// Logical key range for an idempotent load batch (half-open where End is exclusive
// for warehouse ids; table-specific encoding is adapter-local).
struct TLoadKeyRange {
    std::string Table;
    int64_t Begin = 0;
    int64_t End = 0;
};

enum class EPutBatchOutcome {
    Completed,
    OutcomeUnknown,
    Failed,
};

struct TPutBatchResult {
    EPutBatchOutcome Outcome = EPutBatchOutcome::Failed;
    std::string NativeCode;
    std::string Message;
};

// Adapter-owned bulk load surface (specification §6 / adapter-api §4.2).
// Rows are opaque to the shared loader: serialized logical rows or adapter-
// specific buffers produced by the generator + adapter binder.
class ILoadAdapter {
public:
    virtual ~ILoadAdapter() = default;

    virtual TPutBatchResult PutBatch(
        const std::string& runId,
        const TLoadKeyRange& keyRange,
        const std::vector<std::string>& rows) = 0;
};

} // namespace NTpcc
