#pragma once

#include "ydb_driver.h"

#include <admin_adapter.h>

namespace NTpcc {

class TYdbAdminAdapter final : public IAdminAdapter {
public:
    TYdbAdminAdapter(TYdbConnectionConfig connectionConfig, int warehouseCount);

    void EnsureSchema() override;
    void EnsureIndexes() override;
    void EnsureStatistics() override;
    void Clean() override;
    TAdminDescribe Describe() override;

private:
    TYdbConnectionConfig ConnectionConfig_;
    int WarehouseCount_ = 1;
};

} // namespace NTpcc
