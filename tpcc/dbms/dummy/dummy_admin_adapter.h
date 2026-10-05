#pragma once

#include <admin_adapter.h>

namespace NTpcc {

class TDummyAdminAdapter final : public IAdminAdapter {
public:
    void EnsureSchema() override;
    void EnsureIndexes() override;
    void EnsureStatistics() override;
    void Clean() override;
    TAdminDescribe Describe() override;
};

} // namespace NTpcc
