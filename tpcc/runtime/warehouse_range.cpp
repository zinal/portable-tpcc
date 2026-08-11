#include "warehouse_range.h"

#include <stdexcept>
#include <string>

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

std::string FormatWarehouseRanges(const std::vector<TWarehouseRange>& ranges) {
    if (ranges.empty()) {
        return "[]";
    }
    std::string out;
    for (size_t i = 0; i < ranges.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        out.push_back('[');
        out += std::to_string(ranges[i].Start);
        out.push_back(',');
        out += std::to_string(ranges[i].End);
        out.push_back(')');
    }
    return out;
}

int RangeStartInclusive(const TWarehouseRange& range) {
    return range.Start;
}

int RangeEndInclusive(const TWarehouseRange& range) {
    return range.End - 1;
}

} // namespace NTpcc
