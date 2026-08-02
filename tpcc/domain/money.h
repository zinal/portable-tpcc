#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace NTpcc {

// Exact money amount with 2 decimal places (1.00 == 100 units).
// MUST NOT convert through double on the domain↔adapter path.
class TMoney {
public:
    static constexpr int64_t Scale = 100;

    constexpr TMoney() = default;

    static constexpr TMoney FromUnits(int64_t units) {
        return TMoney(units);
    }

    static constexpr TMoney FromCents(int64_t cents) {
        return TMoney(cents);
    }

    // Whole major units and minor (cents), e.g. FromMajorMinor(12, 34) == 12.34
    static constexpr TMoney FromMajorMinor(int64_t major, int64_t minor) {
        return TMoney(major * Scale + minor);
    }

    static TMoney Parse(const std::string& text) {
        if (text.empty()) {
            throw std::invalid_argument("empty money string");
        }
        size_t i = 0;
        bool negative = false;
        if (text[i] == '-') {
            negative = true;
            ++i;
        } else if (text[i] == '+') {
            ++i;
        }
        int64_t major = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            major = major * 10 + (text[i] - '0');
            ++i;
        }
        int64_t minor = 0;
        int digits = 0;
        if (i < text.size() && text[i] == '.') {
            ++i;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 2) {
                minor = minor * 10 + (text[i] - '0');
                ++digits;
                ++i;
            }
            // Truncate/ignore extra fractional digits beyond scale for Parse of DB text
            // that may carry more precision; remaining digits are skipped.
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                ++i;
            }
            while (digits < 2) {
                minor *= 10;
                ++digits;
            }
        }
        if (i != text.size()) {
            throw std::invalid_argument("invalid money string: " + text);
        }
        int64_t units = major * Scale + minor;
        if (negative) {
            units = -units;
        }
        return TMoney(units);
    }

    constexpr int64_t Units() const {
        return Units_;
    }

    constexpr int64_t Cents() const {
        return Units_;
    }

    std::string ToString() const {
        int64_t v = Units_;
        bool negative = v < 0;
        if (negative) {
            v = -v;
        }
        int64_t major = v / Scale;
        int64_t minor = v % Scale;
        std::string out;
        if (negative) {
            out.push_back('-');
        }
        out += std::to_string(major);
        out.push_back('.');
        if (minor < 10) {
            out.push_back('0');
        }
        out += std::to_string(minor);
        return out;
    }

    constexpr TMoney operator+(TMoney other) const {
        return TMoney(Units_ + other.Units_);
    }

    constexpr TMoney operator-(TMoney other) const {
        return TMoney(Units_ - other.Units_);
    }

    constexpr TMoney& operator+=(TMoney other) {
        Units_ += other.Units_;
        return *this;
    }

    constexpr TMoney& operator-=(TMoney other) {
        Units_ -= other.Units_;
        return *this;
    }

    constexpr bool operator==(TMoney other) const {
        return Units_ == other.Units_;
    }

    constexpr bool operator!=(TMoney other) const {
        return Units_ != other.Units_;
    }

    constexpr bool operator<(TMoney other) const {
        return Units_ < other.Units_;
    }

    constexpr bool operator<=(TMoney other) const {
        return Units_ <= other.Units_;
    }

    constexpr bool operator>(TMoney other) const {
        return Units_ > other.Units_;
    }

    constexpr bool operator>=(TMoney other) const {
        return Units_ >= other.Units_;
    }

private:
    explicit constexpr TMoney(int64_t units)
        : Units_(units)
    {}

    int64_t Units_ = 0;
};

// Exact rate (tax, discount) with 4 decimal places (0.1000 == 1000 units).
class TRate {
public:
    static constexpr int64_t Scale = 10000;

    constexpr TRate() = default;

    static constexpr TRate FromUnits(int64_t units) {
        return TRate(units);
    }

    // e.g. FromPermille(100) == 0.1000 (10%)
    static constexpr TRate FromPermille(int64_t permille) {
        return TRate(permille * 10);
    }

    static TRate Parse(const std::string& text) {
        if (text.empty()) {
            throw std::invalid_argument("empty rate string");
        }
        size_t i = 0;
        bool negative = false;
        if (text[i] == '-') {
            negative = true;
            ++i;
        } else if (text[i] == '+') {
            ++i;
        }
        int64_t major = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            major = major * 10 + (text[i] - '0');
            ++i;
        }
        int64_t minor = 0;
        int digits = 0;
        if (i < text.size() && text[i] == '.') {
            ++i;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 4) {
                minor = minor * 10 + (text[i] - '0');
                ++digits;
                ++i;
            }
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
                ++i;
            }
            while (digits < 4) {
                minor *= 10;
                ++digits;
            }
        }
        if (i != text.size()) {
            throw std::invalid_argument("invalid rate string: " + text);
        }
        int64_t units = major * Scale + minor;
        if (negative) {
            units = -units;
        }
        return TRate(units);
    }

    constexpr int64_t Units() const {
        return Units_;
    }

    std::string ToString() const {
        int64_t v = Units_;
        bool negative = v < 0;
        if (negative) {
            v = -v;
        }
        int64_t major = v / Scale;
        int64_t minor = v % Scale;
        std::string out;
        if (negative) {
            out.push_back('-');
        }
        out += std::to_string(major);
        out.push_back('.');
        std::string frac = std::to_string(minor);
        if (frac.size() < 4) {
            out.append(4 - frac.size(), '0');
        }
        out += frac;
        return out;
    }

    constexpr bool operator==(TRate other) const {
        return Units_ == other.Units_;
    }

    constexpr bool operator!=(TRate other) const {
        return Units_ != other.Units_;
    }

private:
    explicit constexpr TRate(int64_t units)
        : Units_(units)
    {}

    int64_t Units_ = 0;
};

} // namespace NTpcc
