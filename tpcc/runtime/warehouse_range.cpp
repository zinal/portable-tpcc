#include "warehouse_range.h"

#include <stdexcept>

namespace NTpcc {

size_t CountWarehouses(const std::vector<TWarehouseRange>& ranges) {
    size_t total = 0;
    for (const auto& r : ranges) {
        if (r.End <= r.Start) {
            throw std::runtime_error("invalid warehouse range");
        }
        total += static_cast<size_t>(r.End - r.Start);
    }
    return total;
}

bool WarehouseInRanges(int warehouseId, const std::vector<TWarehouseRange>& ranges) {
    for (const auto& r : ranges) {
        if (warehouseId >= r.Start && warehouseId < r.End) {
            return true;
        }
    }
    return false;
}

int RangeStartInclusive(const TWarehouseRange& range) {
    return range.Start;
}

int RangeEndInclusive(const TWarehouseRange& range) {
    return range.End - 1;
}

} // namespace NTpcc
