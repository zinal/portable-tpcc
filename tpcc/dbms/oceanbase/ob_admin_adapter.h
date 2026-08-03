#pragma once

#include "schema_options.h"

#include <admin_adapter.h>

#include <string>

namespace NTpcc {

class TObAdminAdapter final : public IAdminAdapter {
public:
    TObAdminAdapter(
        std::string connectionString,
        std::string path,
        TObSchemaOptions schemaOptions = {});

    void EnsureSchema() override;
    void EnsureIndexes() override;
    void EnsureStatistics() override;
    void Clean() override;
    TAdminDescribe Describe() override;

private:
    std::string ConnectionString_;
    std::string Path_;
    TObSchemaOptions SchemaOptions_;
};

} // namespace NTpcc
