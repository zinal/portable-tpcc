#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace NTpcc {

// parameter-reference.md: data.batch_rows is 2000 when omitted or ≤ 0.
// mind-tpcc materializes this into run-config.json; adapters MUST apply the
// same fallback for standalone import and a run-config that still has 0.
constexpr int DEFAULT_LOAD_BATCH_ROWS = 2000;

inline int EffectiveLoadBatchRows(int batchRows) {
    return batchRows > 0 ? batchRows : DEFAULT_LOAD_BATCH_ROWS;
}

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
