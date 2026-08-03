#pragma once

#include "ob_params.h"
#include "ob_queries.h"
#include "query_result.h"

#include <memory>
#include <string>

namespace NTpcc {

class TObStatementCache {
public:
    explicit TObStatementCache(void* mysql);
    ~TObStatementCache();

    TObStatementCache(const TObStatementCache&) = delete;
    TObStatementCache& operator=(const TObStatementCache&) = delete;

    QueryResult Query(EObQueryId id, const TObParams& params);
    uint64_t Execute(EObQueryId id, const TObParams& params);

    QueryResult QueryText(const std::string& sql, const TObParams& params);
    uint64_t ExecuteText(const std::string& sql, const TObParams& params);

    void Clear();

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

} // namespace NTpcc
