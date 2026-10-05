#include "import.h"

#include <log.h>

namespace NTpcc {

void ImportSync(const TImportConfig& config) {
    size_t warehouses = config.WarehouseCount;
    if (!config.WarehouseRanges.empty()) {
        warehouses = 0;
        for (const auto& range : config.WarehouseRanges) {
            warehouses += static_cast<size_t>(range.End - range.Start);
        }
    }
    LOG_I("Dummy import: no data written (warehouses=" << warehouses
          << ", owns_global=" << (config.OwnsGlobalData ? "true" : "false")
          << ", run_id=" << config.RunId << ")");
}

} // namespace NTpcc
