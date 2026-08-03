#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace NTpcc {

class TObParams {
public:
    TObParams() = default;

    TObParams& operator()(std::nullptr_t) {
        Values_.emplace_back(TNull{});
        return *this;
    }

    TObParams& operator()(int32_t v) {
        Values_.emplace_back(v);
        return *this;
    }

    TObParams& operator()(int64_t v) {
        Values_.emplace_back(v);
        return *this;
    }

    TObParams& operator()(uint64_t v) {
        Values_.emplace_back(v);
        return *this;
    }

    TObParams& operator()(double v) {
        Values_.emplace_back(v);
        return *this;
    }

    TObParams& operator()(std::string_view v) {
        Values_.emplace_back(std::string(v));
        return *this;
    }

    TObParams& operator()(const char* v) {
        Values_.emplace_back(std::string(v ? v : ""));
        return *this;
    }

    TObParams& operator()(const std::string& v) {
        Values_.emplace_back(v);
        return *this;
    }

    struct TTimestamp {
        int Year = 0;
        int Month = 0;
        int Day = 0;
        int Hour = 0;
        int Minute = 0;
        int Second = 0;
    };

    TObParams& operator()(TTimestamp v) {
        Values_.emplace_back(v);
        return *this;
    }

    struct TNull {};
    using TValue = std::variant<TNull, int32_t, int64_t, uint64_t, double, std::string, TTimestamp>;

    const std::vector<TValue>& Values() const {
        return Values_;
    }

    size_t Size() const {
        return Values_.size();
    }

    bool Empty() const {
        return Values_.empty();
    }

private:
    std::vector<TValue> Values_;
};

template <typename... Args>
inline TObParams MakeParams(Args&&... args) {
    TObParams params;
    (params(std::forward<Args>(args)), ...);
    return params;
}

} // namespace NTpcc
