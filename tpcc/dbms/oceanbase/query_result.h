#pragma once

#include <money.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace NTpcc {

class QueryResult {
public:
    QueryResult() = default;

    QueryResult(
        std::vector<std::string> columns,
        std::vector<std::vector<std::optional<std::string>>> rows)
        : Columns_(std::move(columns))
        , Rows_(std::move(rows))
    {
        for (size_t i = 0; i < Columns_.size(); ++i) {
            ColumnIndex_[Columns_[i]] = i;
        }
    }

    bool TryNextRow() {
        if (RowPos_ + 1 >= Rows_.size()) {
            return false;
        }
        ++RowPos_;
        return true;
    }

    void Reset() {
        RowPos_ = static_cast<size_t>(-1);
    }

    size_t GetRowsCount() const {
        return Rows_.size();
    }

    bool IsEmpty() const {
        return Rows_.empty();
    }

    int32_t GetInt32(std::string_view col) const {
        return static_cast<int32_t>(GetInt64(col));
    }

    int64_t GetInt64(std::string_view col) const {
        return std::stoll(Require(IndexOf(col), std::string(col)));
    }

    uint64_t GetUint64(std::string_view col) const {
        return std::stoull(Require(IndexOf(col), std::string(col)));
    }

    double GetDouble(std::string_view col) const {
        return std::stod(Require(IndexOf(col), std::string(col)));
    }

    std::string GetString(std::string_view col) const {
        return Require(IndexOf(col), std::string(col));
    }

    std::optional<int32_t> GetOptionalInt32(std::string_view col) const {
        auto s = Optional(IndexOf(col));
        if (!s) return std::nullopt;
        return static_cast<int32_t>(std::stoll(*s));
    }

    std::optional<int64_t> GetOptionalInt64(std::string_view col) const {
        auto s = Optional(IndexOf(col));
        if (!s) return std::nullopt;
        return std::stoll(*s);
    }

    std::optional<uint64_t> GetOptionalUint64(std::string_view col) const {
        auto s = Optional(IndexOf(col));
        if (!s) return std::nullopt;
        return std::stoull(*s);
    }

    std::optional<double> GetOptionalDouble(std::string_view col) const {
        auto s = Optional(IndexOf(col));
        if (!s) return std::nullopt;
        return std::stod(*s);
    }

    std::optional<std::string> GetOptionalString(std::string_view col) const {
        return Optional(IndexOf(col));
    }

    TMoney GetMoney(std::string_view col) const {
        return TMoney::Parse(GetString(col));
    }

    TRate GetRate(std::string_view col) const {
        return TRate::Parse(GetString(col));
    }

    int32_t GetInt32(size_t col) const {
        return static_cast<int32_t>(GetInt64(col));
    }

    int64_t GetInt64(size_t col) const {
        return std::stoll(Require(col, std::to_string(col)));
    }

    uint64_t GetUint64(size_t col) const {
        return std::stoull(Require(col, std::to_string(col)));
    }

    double GetDouble(size_t col) const {
        return std::stod(Require(col, std::to_string(col)));
    }

    std::string GetString(size_t col) const {
        return Require(col, std::to_string(col));
    }

    std::optional<int32_t> GetOptionalInt32(size_t col) const {
        auto s = Optional(col);
        if (!s) return std::nullopt;
        return static_cast<int32_t>(std::stoll(*s));
    }

    std::optional<int64_t> GetOptionalInt64(size_t col) const {
        auto s = Optional(col);
        if (!s) return std::nullopt;
        return std::stoll(*s);
    }

    std::optional<std::string> GetOptionalString(size_t col) const {
        return Optional(col);
    }

    TMoney GetMoney(size_t col) const {
        return TMoney::Parse(GetString(col));
    }

    TRate GetRate(size_t col) const {
        return TRate::Parse(GetString(col));
    }

private:
    size_t IndexOf(std::string_view col) const {
        auto it = ColumnIndex_.find(std::string(col));
        if (it == ColumnIndex_.end()) {
            throw std::runtime_error("Unknown column: " + std::string(col));
        }
        return it->second;
    }

    void EnsureRow() const {
        if (RowPos_ >= Rows_.size()) {
            throw std::runtime_error("No current row in QueryResult");
        }
    }

    std::string Require(size_t col, const std::string& name) const {
        EnsureRow();
        if (col >= Rows_[RowPos_].size()) {
            throw std::runtime_error("Column out of range: " + name);
        }
        const auto& cell = Rows_[RowPos_][col];
        if (!cell) {
            throw std::runtime_error("NULL in column: " + name);
        }
        return *cell;
    }

    std::optional<std::string> Optional(size_t col) const {
        EnsureRow();
        if (col >= Rows_[RowPos_].size()) {
            throw std::runtime_error("Column out of range: " + std::to_string(col));
        }
        return Rows_[RowPos_][col];
    }

    std::vector<std::string> Columns_;
    std::vector<std::vector<std::optional<std::string>>> Rows_;
    std::unordered_map<std::string, size_t> ColumnIndex_;
    size_t RowPos_ = static_cast<size_t>(-1);
};

} // namespace NTpcc
