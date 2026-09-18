#pragma once

#include <cstdint>
#include <vector>

namespace NTpcc {

class THistogram {
public:
    // Linear [0, hdrTill) at unit resolution, then at most kSubBucketsPerOctave
    // equal-width sub-buckets per doubling octave up to maxValue. Values at or
    // above maxValue increment OverflowCount and are not stored in buckets.
    static constexpr size_t kSubBucketsPerOctave = 64;

    THistogram(uint64_t hdrTill, uint64_t maxValue);

    void RecordValue(uint64_t value);
    void Add(const THistogram& other);
    void Sub(const THistogram& other);
    uint64_t GetValueAtPercentile(double percentile) const;
    void Reset();

    uint64_t HdrTill() const { return HdrTill_; }
    uint64_t MaxValue() const { return MaxValue_; }
    uint64_t TotalCount() const { return TotalCount_; }
    uint64_t OverflowCount() const { return OverflowCount_; }
    // Exact recorded maximum; 0 when empty.
    uint64_t MaxRecordedValue() const { return MaxRecordedValue_; }
    // Exact recorded minimum; 0 when empty.
    uint64_t MinRecordedValue() const {
        return TotalCount_ == 0 ? 0 : MinRecordedValue_;
    }
    // Exact sum of recorded values (for average = SumValues / TotalCount).
    uint64_t SumValues() const { return SumValues_; }

    const std::vector<uint64_t>& Buckets() const { return Buckets_; }

private:
    template <typename F>
    void ForEachOctave(F&& fn) const;
    size_t GetBucketIndex(uint64_t value) const;
    uint64_t GetBucketUpperBound(size_t bucketIndex) const;
    size_t GetTotalBuckets() const;

private:
    uint64_t HdrTill_;
    uint64_t MaxValue_;
    std::vector<uint64_t> Buckets_;
    uint64_t TotalCount_;
    uint64_t OverflowCount_;
    uint64_t MaxRecordedValue_;
    uint64_t MinRecordedValue_;
    uint64_t SumValues_;
};

} // namespace NTpcc
