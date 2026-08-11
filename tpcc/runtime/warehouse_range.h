#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace NTpcc {

// Half-open warehouse interval [Start, End) per portable-tpcc assignment.
struct TWarehouseRange {
    int Start = 0;
    int End = 0;
};

size_t CountWarehouses(const std::vector<TWarehouseRange>& ranges);
bool WarehouseInRanges(int warehouseId, const std::vector<TWarehouseRange>& ranges);

// Human-readable half-open ranges, e.g. "[1,1801),[1801,3601)".
std::string FormatWarehouseRanges(const std::vector<TWarehouseRange>& ranges);

// Converts half-open [start, end) to inclusive [start, end-1] for legacy loaders.
int RangeStartInclusive(const TWarehouseRange& range);
int RangeEndInclusive(const TWarehouseRange& range);

} // namespace NTpcc
