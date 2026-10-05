#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

namespace NTpcc {

// Fixed-bucket latency histogram rendered as a Prometheus histogram.
// Bounds are upper edges in microseconds; the last Prometheus bucket is +Inf.
class TPromHistogram {
public:
    static constexpr size_t kBoundCount = 16;

    void RecordMicros(int64_t micros);
    void Add(const TPromHistogram& other);
    void Reset();

    uint64_t Count() const { return Count_; }
    uint64_t Overflow() const { return Overflow_; }
    uint64_t SumMicros() const { return SumMicros_; }
    // Non-cumulative count of samples that fell into this bound
    // (previous bound, this bound].
    uint64_t Bucket(size_t index) const { return Buckets_[index]; }

    static uint64_t BoundMicros(size_t index);
    // Prometheus `le` label without quotes, e.g. "0.005" or "+Inf" is not here.
    static const char* BoundLabel(size_t index);

private:
    std::array<uint64_t, kBoundCount> Buckets_{};
    uint64_t Overflow_ = 0;
    uint64_t Count_ = 0;
    uint64_t SumMicros_ = 0;
};

// One transaction type over a single collection interval.
struct TPromTxSnapshot {
    uint64_t Success = 0;
    uint64_t Failure = 0;
    uint64_t Retries = 0;
    uint64_t Rollbacks = 0;
    TPromHistogram SuccessLatency;
    TPromHistogram FailureLatency;
    TPromHistogram AdmissionWait;
    TPromHistogram SessionPoolWait;
    TPromHistogram RetryBackoff;
};

} // namespace NTpcc
