#include "ob_prepared_statement.h"

#include "ob_errors.h"

#include <mysql.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace NTpcc {
namespace {

[[noreturn]] void ThrowStmtError(MYSQL_STMT* stmt, const char* what) {
    const int code = stmt ? static_cast<int>(mysql_stmt_errno(stmt)) : 0;
    const char* msg = stmt ? mysql_stmt_error(stmt) : "null stmt handle";
    throw TObDbError(code, std::string(what) + ": [" + std::to_string(code) + "] " + msg);
}

constexpr size_t RESULT_BUFFER_SIZE = 65536;

struct TParamBinding {
    enum class EKind { Null, Int32, Int64, Uint64, Double, String, Timestamp } Kind = EKind::Null;

    int32_t I32 = 0;
    int64_t I64 = 0;
    uint64_t U64 = 0;
    double Dbl = 0;
    std::string Str;
    unsigned long StrLength = 0;
    MYSQL_TIME Ts{};
    MYSQL_BIND Bind{};

    void ResetBind() {
        std::memset(&Bind, 0, sizeof(Bind));
    }
};

void SetupParamBind(TParamBinding& slot) {
    slot.ResetBind();
    switch (slot.Kind) {
        case TParamBinding::EKind::Null:
            slot.Bind.buffer_type = MYSQL_TYPE_NULL;
            break;
        case TParamBinding::EKind::Int32:
            slot.Bind.buffer_type = MYSQL_TYPE_LONG;
            slot.Bind.buffer = &slot.I32;
            slot.Bind.is_unsigned = false;
            break;
        case TParamBinding::EKind::Int64:
            slot.Bind.buffer_type = MYSQL_TYPE_LONGLONG;
            slot.Bind.buffer = &slot.I64;
            slot.Bind.is_unsigned = false;
            break;
        case TParamBinding::EKind::Uint64:
            slot.Bind.buffer_type = MYSQL_TYPE_LONGLONG;
            slot.Bind.buffer = &slot.U64;
            slot.Bind.is_unsigned = true;
            break;
        case TParamBinding::EKind::Double:
            slot.Bind.buffer_type = MYSQL_TYPE_DOUBLE;
            slot.Bind.buffer = &slot.Dbl;
            break;
        case TParamBinding::EKind::String:
            slot.Bind.buffer_type = MYSQL_TYPE_STRING;
            slot.StrLength = static_cast<unsigned long>(slot.Str.size());
            slot.Bind.buffer = slot.Str.data();
            slot.Bind.buffer_length = slot.StrLength;
            slot.Bind.length = &slot.StrLength;
            break;
        case TParamBinding::EKind::Timestamp:
            slot.Bind.buffer_type = MYSQL_TYPE_TIMESTAMP;
            slot.Bind.buffer = &slot.Ts;
            break;
    }
}

void FillParamSlot(TParamBinding& slot, const TObParams::TValue& value) {
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, TObParams::TNull>) {
                slot.Kind = TParamBinding::EKind::Null;
            } else if constexpr (std::is_same_v<T, int32_t>) {
                slot.Kind = TParamBinding::EKind::Int32;
                slot.I32 = v;
            } else if constexpr (std::is_same_v<T, int64_t>) {
                slot.Kind = TParamBinding::EKind::Int64;
                slot.I64 = v;
            } else if constexpr (std::is_same_v<T, uint64_t>) {
                slot.Kind = TParamBinding::EKind::Uint64;
                slot.U64 = v;
            } else if constexpr (std::is_same_v<T, double>) {
                slot.Kind = TParamBinding::EKind::Double;
                slot.Dbl = v;
            } else if constexpr (std::is_same_v<T, std::string>) {
                slot.Kind = TParamBinding::EKind::String;
                slot.Str = v;
            } else if constexpr (std::is_same_v<T, TObParams::TTimestamp>) {
                slot.Kind = TParamBinding::EKind::Timestamp;
                slot.Ts.year = v.Year;
                slot.Ts.month = static_cast<unsigned int>(v.Month);
                slot.Ts.day = static_cast<unsigned int>(v.Day);
                slot.Ts.hour = static_cast<unsigned int>(v.Hour);
                slot.Ts.minute = static_cast<unsigned int>(v.Minute);
                slot.Ts.second = static_cast<unsigned int>(v.Second);
            }
        },
        value);
    SetupParamBind(slot);
}

void BindParams(
    MYSQL_STMT* stmt,
    const TObParams& params,
    std::vector<TParamBinding>& slots,
    std::vector<MYSQL_BIND>& binds)
{
    slots.resize(params.Size());
    binds.resize(params.Size());
    for (size_t i = 0; i < params.Size(); ++i) {
        FillParamSlot(slots[i], params.Values()[i]);
        binds[i] = slots[i].Bind;
    }
    if (!params.Empty() && mysql_stmt_bind_param(stmt, binds.data()) != 0) {
        ThrowStmtError(stmt, "mysql_stmt_bind_param failed");
    }
}

struct TStmtGuard {
    MYSQL_STMT* Stmt = nullptr;

    ~TStmtGuard() {
        if (Stmt) {
            mysql_stmt_close(Stmt);
        }
    }
};

MYSQL_STMT* PrepareTextStmt(MYSQL* mysql, const std::string& sql) {
    MYSQL_STMT* stmt = mysql_stmt_init(mysql);
    if (!stmt) {
        throw std::runtime_error("mysql_stmt_init failed");
    }
    if (mysql_stmt_prepare(stmt, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        const int code = static_cast<int>(mysql_stmt_errno(stmt));
        const std::string msg = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        throw TObDbError(
            code,
            std::string("mysql_stmt_prepare failed: [") + std::to_string(code) + "] " + msg);
    }
    return stmt;
}

QueryResult MaterializeStmtResult(MYSQL_STMT* stmt, MYSQL_RES* meta) {
    if (!meta) {
        return QueryResult{};
    }

    const unsigned numFields = mysql_num_fields(meta);
    MYSQL_FIELD* fields = mysql_fetch_fields(meta);

    std::vector<std::string> columns;
    columns.reserve(numFields);
    for (unsigned i = 0; i < numFields; ++i) {
        columns.emplace_back(fields[i].name ? fields[i].name : "");
    }

    std::vector<std::vector<char>> buffers(numFields);
    std::vector<unsigned long> lengths(numFields);
    std::vector<my_bool> isNull(numFields);
    std::vector<MYSQL_BIND> resultBinds(numFields);

    for (unsigned i = 0; i < numFields; ++i) {
        buffers[i].assign(RESULT_BUFFER_SIZE, '\0');
        std::memset(&resultBinds[i], 0, sizeof(MYSQL_BIND));
        resultBinds[i].buffer_type = MYSQL_TYPE_STRING;
        resultBinds[i].buffer = buffers[i].data();
        resultBinds[i].buffer_length = RESULT_BUFFER_SIZE;
        resultBinds[i].length = &lengths[i];
        resultBinds[i].is_null = &isNull[i];
    }

    if (mysql_stmt_bind_result(stmt, resultBinds.data()) != 0) {
        mysql_free_result(meta);
        ThrowStmtError(stmt, "mysql_stmt_bind_result failed");
    }

    std::vector<std::vector<std::optional<std::string>>> rows;
    while (true) {
        const int rc = mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA) {
            break;
        }
        if (rc != 0 && rc != MYSQL_DATA_TRUNCATED) {
            mysql_free_result(meta);
            ThrowStmtError(stmt, "mysql_stmt_fetch failed");
        }

        std::vector<std::optional<std::string>> row;
        row.reserve(numFields);
        for (unsigned i = 0; i < numFields; ++i) {
            if (isNull[i]) {
                row.emplace_back(std::nullopt);
            } else {
                row.emplace_back(std::string(buffers[i].data(), lengths[i]));
            }
        }
        rows.push_back(std::move(row));
    }

    mysql_free_result(meta);
    return QueryResult(std::move(columns), std::move(rows));
}

} // namespace

struct TObStatementCache::TImpl {
    struct TStmtEntry {
        MYSQL_STMT* Stmt = nullptr;
        bool Prepared = false;
        std::vector<TParamBinding> ParamSlots;
        std::vector<MYSQL_BIND> ParamBinds;
    };

    MYSQL* Mysql = nullptr;
    TStmtEntry Entries[static_cast<size_t>(EObQueryId::Count)];

    explicit TImpl(void* mysql)
        : Mysql(static_cast<MYSQL*>(mysql))
    {}

    TStmtEntry& Get(EObQueryId id) {
        return Entries[static_cast<size_t>(id)];
    }

    void Prepare(TStmtEntry& entry, EObQueryId id) {
        if (entry.Prepared) {
            return;
        }
        entry.Stmt = mysql_stmt_init(Mysql);
        if (!entry.Stmt) {
            throw std::runtime_error("mysql_stmt_init failed");
        }
        const std::string_view sql = QuerySql(id);
        if (mysql_stmt_prepare(entry.Stmt, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
            ThrowStmtError(entry.Stmt, "mysql_stmt_prepare failed");
        }
        entry.Prepared = true;
    }

    void Clear() {
        for (size_t i = 0; i < static_cast<size_t>(EObQueryId::Count); ++i) {
            if (Entries[i].Stmt) {
                mysql_stmt_close(Entries[i].Stmt);
                Entries[i].Stmt = nullptr;
            }
            Entries[i].Prepared = false;
            Entries[i].ParamSlots.clear();
            Entries[i].ParamBinds.clear();
        }
    }
};

TObStatementCache::TObStatementCache(void* mysql)
    : Impl_(std::make_unique<TImpl>(mysql))
{}

TObStatementCache::~TObStatementCache() {
    Clear();
}

void TObStatementCache::Clear() {
    if (Impl_) {
        Impl_->Clear();
    }
}

QueryResult TObStatementCache::Query(EObQueryId id, const TObParams& params) {
    auto& entry = Impl_->Get(id);
    Impl_->Prepare(entry, id);
    BindParams(entry.Stmt, params, entry.ParamSlots, entry.ParamBinds);
    if (mysql_stmt_execute(entry.Stmt) != 0) {
        ThrowStmtError(entry.Stmt, "mysql_stmt_execute failed");
    }
    MYSQL_RES* meta = mysql_stmt_result_metadata(entry.Stmt);
    return MaterializeStmtResult(entry.Stmt, meta);
}

uint64_t TObStatementCache::Execute(EObQueryId id, const TObParams& params) {
    auto& entry = Impl_->Get(id);
    Impl_->Prepare(entry, id);
    BindParams(entry.Stmt, params, entry.ParamSlots, entry.ParamBinds);
    if (mysql_stmt_execute(entry.Stmt) != 0) {
        ThrowStmtError(entry.Stmt, "mysql_stmt_execute failed");
    }
    return static_cast<uint64_t>(mysql_stmt_affected_rows(entry.Stmt));
}

QueryResult TObStatementCache::QueryText(const std::string& sql, const TObParams& params) {
    TStmtGuard guard{PrepareTextStmt(Impl_->Mysql, sql)};
    std::vector<TParamBinding> slots;
    std::vector<MYSQL_BIND> binds;
    BindParams(guard.Stmt, params, slots, binds);
    if (mysql_stmt_execute(guard.Stmt) != 0) {
        ThrowStmtError(guard.Stmt, "mysql_stmt_execute failed");
    }
    MYSQL_RES* meta = mysql_stmt_result_metadata(guard.Stmt);
    return MaterializeStmtResult(guard.Stmt, meta);
}

uint64_t TObStatementCache::ExecuteText(const std::string& sql, const TObParams& params) {
    TStmtGuard guard{PrepareTextStmt(Impl_->Mysql, sql)};
    std::vector<TParamBinding> slots;
    std::vector<MYSQL_BIND> binds;
    BindParams(guard.Stmt, params, slots, binds);
    if (mysql_stmt_execute(guard.Stmt) != 0) {
        ThrowStmtError(guard.Stmt, "mysql_stmt_execute failed");
    }
    return static_cast<uint64_t>(mysql_stmt_affected_rows(guard.Stmt));
}

} // namespace NTpcc
