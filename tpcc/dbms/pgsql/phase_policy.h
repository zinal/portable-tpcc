#pragma once

#include <cstdint>

namespace NTpcc {

struct TPhasePolicy {
    int64_t StartLeadMs = 0;
    int64_t RampUpMs = 0;
    int64_t MeasurementMs = 0;
    int64_t TransactionDrainMs = 0;
    int64_t StopGraceMs = 0;
};

} // namespace NTpcc
