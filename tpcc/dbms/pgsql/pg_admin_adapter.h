#pragma once

#include <admin_adapter.h>

#include <string>

namespace NTpcc {

class TPgAdminAdapter final : public IAdminAdapter {
public:
    TPgAdminAdapter(std::string connectionString, std::string path);

    void EnsureSchema() override;
    void EnsureIndexes() override;
    void EnsureStatistics() override;
    void Clean() override;
    TAdminDescribe Describe() override;

private:
    std::string ConnectionString_;
    std::string Path_;
};

} // namespace NTpcc
