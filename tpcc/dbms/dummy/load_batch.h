#pragma once

#include <put_batch.h>

namespace NTpcc {

class TDummyLoadAdapter : public ILoadAdapter {
public:
    TPutBatchResult PutBatch(
        const std::string& runId,
        const TLoadKeyRange& keyRange,
        const std::vector<std::string>& rows) override;
};

} // namespace NTpcc
