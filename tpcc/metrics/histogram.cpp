#include "histogram.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace NTpcc {

namespace {

size_t SubBucketCount(uint64_t span) {
    if (span == 0) {
        return 1;
    }
    if (span < THistogram::kSubBucketsPerOctave) {
        return static_cast<size_t>(span);
    }
    return THistogram::kSubBucketsPerOctave;
}

uint64_t AddSaturating(uint64_t a, uint64_t b) {
    if (b > std::numeric_limits<uint64_t>::max() - a) {
        return std::numeric_limits<uint64_t>::max();
    }
    return a + b;
}

} // namespace

THistogram::THistogram(uint64_t hdrTill, uint64_t maxValue)
    : HdrTill_(hdrTill)
    , MaxValue_(maxValue)
    , TotalCount_(0)
    , OverflowCount_(0)
    , MaxRecordedValue_(0)
    , MinRecordedValue_(std::numeric_limits<uint64_t>::max())
    , SumValues_(0)
{
    if (hdrTill == 0 || maxValue == 0 || hdrTill > maxValue) {
        throw std::invalid_argument("Invalid histogram parameters");
    }

    size_t totalBuckets = GetTotalBuckets();
    Buckets_.resize(totalBuckets, 0);
}

template <typename F>
void THistogram::ForEachOctave(F&& fn) const {
    uint64_t start = HdrTill_;
    uint64_t size = HdrTill_;
    while (start < MaxValue_) {
        const uint64_t end = std::min(AddSaturating(start, size), MaxValue_);
        const uint64_t span = end - start;
        const size_t nsubs = SubBucketCount(span);
        fn(start, end, span, nsubs);
        start = end;
        if (size > std::numeric_limits<uint64_t>::max() / 2) {
            break;
        }
        size *= 2;
    }
}

void THistogram::RecordValue(uint64_t value) {
    TotalCount_++;
    MaxRecordedValue_ = std::max(MaxRecordedValue_, value);
    MinRecordedValue_ = std::min(MinRecordedValue_, value);
    SumValues_ += value;
    if (value >= MaxValue_) {
        OverflowCount_++;
        return;
    }

    size_t bucketIndex = GetBucketIndex(value);
    if (bucketIndex >= Buckets_.size()) {
        throw std::runtime_error("THistogram internal error");
    }
    Buckets_[bucketIndex]++;
}

void THistogram::Add(const THistogram& other) {
    if (HdrTill_ != other.HdrTill_ || MaxValue_ != other.MaxValue_) {
        throw std::invalid_argument("Cannot add histograms with different parameters");
    }

    for (size_t i = 0; i < Buckets_.size() && i < other.Buckets_.size(); ++i) {
        Buckets_[i] += other.Buckets_[i];
    }
    if (other.TotalCount_ > 0) {
        MinRecordedValue_ = std::min(MinRecordedValue_, other.MinRecordedValue_);
    }
    TotalCount_ += other.TotalCount_;
    OverflowCount_ += other.OverflowCount_;
    MaxRecordedValue_ = std::max(MaxRecordedValue_, other.MaxRecordedValue_);
    SumValues_ += other.SumValues_;
}

void THistogram::Sub(const THistogram& other) {
    if (HdrTill_ != other.HdrTill_ || MaxValue_ != other.MaxValue_) {
        throw std::invalid_argument("Cannot sub histograms with different parameters");
    }

    for (size_t i = 0; i < Buckets_.size() && i < other.Buckets_.size(); ++i) {
        Buckets_[i] -= other.Buckets_[i];
    }
    TotalCount_ -= other.TotalCount_;
    OverflowCount_ -= other.OverflowCount_;
    // Min/max of a window delta cannot be restored from Sub; leave absolute extrema.
    SumValues_ -= other.SumValues_;
}

uint64_t THistogram::GetValueAtPercentile(double percentile) const {
    if (percentile < 0.0 || percentile > 100.0) {
        throw std::invalid_argument("Percentile must be between 0 and 100");
    }

    if (TotalCount_ == 0) {
        return 0;
    }

    uint64_t targetCount = static_cast<uint64_t>(std::ceil(percentile * TotalCount_ / 100.0));
    uint64_t currentCount = 0;

    for (size_t i = 0; i < Buckets_.size(); ++i) {
        currentCount += Buckets_[i];
        if (currentCount >= targetCount) {
            return GetBucketUpperBound(i);
        }
    }

    if (OverflowCount_ > 0) {
        currentCount += OverflowCount_;
        if (currentCount >= targetCount) {
            // Censored: do not substitute the exact recorded maximum.
            return MaxValue_;
        }
    }

    return std::numeric_limits<uint64_t>::max();
}

void THistogram::Reset() {
    std::fill(Buckets_.begin(), Buckets_.end(), 0);
    TotalCount_ = 0;
    OverflowCount_ = 0;
    MaxRecordedValue_ = 0;
    MinRecordedValue_ = std::numeric_limits<uint64_t>::max();
    SumValues_ = 0;
}

size_t THistogram::GetBucketIndex(uint64_t value) const {
    if (value < HdrTill_) {
        return static_cast<size_t>(value);
    }

    size_t index = HdrTill_;
    size_t found = Buckets_.size();
    ForEachOctave([&](uint64_t start, uint64_t end, uint64_t span, size_t nsubs) {
        if (found != Buckets_.size()) {
            return;
        }
        if (value >= start && value < end) {
            uint64_t off = value - start;
            size_t sub = static_cast<size_t>((off * static_cast<uint64_t>(nsubs)) / span);
            if (sub >= nsubs) {
                sub = nsubs - 1;
            }
            found = index + sub;
            return;
        }
        index += nsubs;
    });
    if (found == Buckets_.size()) {
        throw std::runtime_error("THistogram internal error");
    }
    return found;
}

uint64_t THistogram::GetBucketUpperBound(size_t bucketIndex) const {
    if (bucketIndex < HdrTill_) {
        return static_cast<uint64_t>(bucketIndex + 1);
    }

    size_t index = HdrTill_;
    uint64_t found = 0;
    bool have = false;
    ForEachOctave([&](uint64_t start, uint64_t end, uint64_t span, size_t nsubs) {
        if (have) {
            return;
        }
        if (bucketIndex < index + nsubs) {
            size_t sub = bucketIndex - index;
            uint64_t upper = start + ((static_cast<uint64_t>(sub) + 1) * span) / nsubs;
            if (upper > end || upper < start) {
                upper = end;
            }
            found = upper;
            have = true;
            return;
        }
        index += nsubs;
    });
    if (!have) {
        return MaxValue_;
    }
    return found;
}

size_t THistogram::GetTotalBuckets() const {
    size_t count = HdrTill_;
    ForEachOctave([&](uint64_t, uint64_t, uint64_t, size_t nsubs) {
        count += nsubs;
    });
    return count;
}

} // namespace NTpcc
