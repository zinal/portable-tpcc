#pragma once

#include <rng.h>

#include <cstddef>

namespace NTpcc {

// Supply warehouse (OL_SUPPLY_W_ID) for one New-Order line.
//
// remoteWarehousesChosen is how many lines in this transaction already use a
// remote supply warehouse. maxRemoteWarehouses == 0 means no cap (TPC-C
// §2.4.1.5: each line is chosen independently). Once the cap is reached, this
// line and every later line use the home warehouse and do not draw another
// remote-warehouse random number.
inline int ChooseNewOrderSupplyWarehouse(
    NDetail::TFastRng& rng,
    int homeWarehouseId,
    size_t warehouseCount,
    int remoteWarehousePercent,
    int maxRemoteWarehouses,
    int remoteWarehousesChosen)
{
    const bool capReached =
        maxRemoteWarehouses > 0 && remoteWarehousesChosen >= maxRemoteWarehouses;
    if (warehouseCount == 1 || capReached ||
        static_cast<int>(RandomNumber(rng, 1, 100)) > remoteWarehousePercent)
    {
        return homeWarehouseId;
    }
    int supplierID;
    do {
        supplierID = static_cast<int>(RandomNumber(rng, 1, warehouseCount));
    } while (supplierID == homeWarehouseId);
    return supplierID;
}

} // namespace NTpcc
