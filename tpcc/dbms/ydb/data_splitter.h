#pragma once

#include <string>
#include <vector>

namespace NTpcc {

class TDataSplitter {
public:
    explicit TDataSplitter(int warehouseCount);

    std::vector<int> GetSplitKeys(const std::string& table) const;
    std::string GetSplitKeysString(const std::string& table) const;

    static int CalcMinParts(int warehouseCount);
    static double GetPerWarehouseMB(const std::string& table);

private:
    int WarehouseCount_;
};

} // namespace NTpcc
