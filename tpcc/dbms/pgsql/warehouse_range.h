#pragma once

#include <cstddef>
#include <vector>

namespace NTpcc {

// Half-open warehouse interval [Start, End) per portable-tpcc assignment.
struct TWarehouseRange {
    int Start = 0;
    int End = 0;
};

size_t CountWarehouses(const std::vector<TWarehouseRange>& ranges);
bool WarehouseInRanges(int warehouseId, const std::vector<TWarehouseRange>& ranges);

// Converts half-open [start, end) to inclusive [start, end-1] for legacy loaders.
int RangeStartInclusive(const TWarehouseRange& range);
int RangeEndInclusive(const TWarehouseRange& range);

} // namespace NTpcc
