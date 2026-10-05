#include "prom_histogram.h"

#include <algorithm>

namespace NTpcc {

namespace {

constexpr uint64_t kBoundsUs[TPromHistogram::kBoundCount] = {
    500,
    1000,
    2000,
    5000,
    10000,
    25000,
    50000,
    100000,
    250000,
    500000,
    1000000,
    2500000,
    5000000,
    10000000,
    20000000,
    60000000,
};

constexpr const char* kBoundLabels[TPromHistogram::kBoundCount] = {
    "0.0005",
    "0.001",
    "0.002",
    "0.005",
    "0.01",
    "0.025",
    "0.05",
    "0.1",
    "0.25",
    "0.5",
    "1",
    "2.5",
    "5",
    "10",
    "20",
    "60",
};

} // namespace

uint64_t TPromHistogram::BoundMicros(size_t index) {
    return kBoundsUs[index];
}

const char* TPromHistogram::BoundLabel(size_t index) {
    return kBoundLabels[index];
}

void TPromHistogram::RecordMicros(int64_t micros) {
    if (micros < 0) {
        micros = 0;
    }
    const uint64_t value = static_cast<uint64_t>(micros);
    ++Count_;
    SumMicros_ += value;
    for (size_t i = 0; i < kBoundCount; ++i) {
        if (value <= kBoundsUs[i]) {
            ++Buckets_[i];
            return;
        }
    }
    ++Overflow_;
}

void TPromHistogram::Add(const TPromHistogram& other) {
    for (size_t i = 0; i < kBoundCount; ++i) {
        Buckets_[i] += other.Buckets_[i];
    }
    Overflow_ += other.Overflow_;
    Count_ += other.Count_;
    SumMicros_ += other.SumMicros_;
}

void TPromHistogram::Reset() {
    Buckets_.fill(0);
    Overflow_ = 0;
    Count_ = 0;
    SumMicros_ = 0;
}

} // namespace NTpcc
