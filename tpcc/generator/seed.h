#pragma once

#include <rng.h>

#include <cstdint>

namespace NTpcc::NGenerator {

// Stable table/stream salts for Mix/Fork. Changing these changes generated data.
constexpr uint64_t SaltItem = 0x4954454DULL;          // 'ITEM'
constexpr uint64_t SaltWarehouse = 0x57485245ULL;     // 'WHRE'
constexpr uint64_t SaltDistrict = 0x44535452ULL;      // 'DSTR'
constexpr uint64_t SaltCustomer = 0x43555354ULL;      // 'CUST'
constexpr uint64_t SaltStock = 0x53544F4BULL;         // 'STOK'
constexpr uint64_t SaltOrder = 0x4F524452ULL;         // 'ORDR'
constexpr uint64_t SaltHistory = 0x48535452ULL;       // 'HSTR'
constexpr uint64_t SaltNewOrder = 0x4E4F5244ULL;      // 'NORD'
constexpr uint64_t SaltOrderLine = 0x4F4C4E45ULL;     // 'OLNE'
constexpr uint64_t SaltTxnInputs = 0x54584E49ULL;     // 'TXNI'

inline TSeededRng RootRng(uint64_t seed) {
    return TSeededRng(seed);
}

inline TSeededRng TableRng(uint64_t seed, uint64_t tableSalt) {
    return RootRng(seed).Fork(tableSalt);
}

inline TSeededRng WarehouseRng(uint64_t seed, uint64_t tableSalt, int warehouseId) {
    return TableRng(seed, tableSalt).Fork(static_cast<uint64_t>(warehouseId));
}

inline TSeededRng DistrictRng(uint64_t seed, uint64_t tableSalt, int warehouseId, int districtId) {
    return WarehouseRng(seed, tableSalt, warehouseId).Fork(static_cast<uint64_t>(districtId));
}

} // namespace NTpcc::NGenerator
