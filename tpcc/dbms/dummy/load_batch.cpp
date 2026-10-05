#include "load_batch.h"

namespace NTpcc {

TPutBatchResult TDummyLoadAdapter::PutBatch(
    const std::string& /*runId*/,
    const TLoadKeyRange& /*keyRange*/,
    const std::vector<std::string>& /*rows*/)
{
    TPutBatchResult result;
    result.Outcome = EPutBatchOutcome::Completed;
    return result;
}

} // namespace NTpcc
