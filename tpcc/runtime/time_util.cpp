#include "time_util.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace NTpcc {

namespace {

bool IsDigitAt(const std::string& text, size_t pos) {
    return pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]));
}

int ParseDigits(const std::string& text, size_t pos, size_t len) {
    int value = 0;
    for (size_t i = 0; i < len; ++i) {
        if (!IsDigitAt(text, pos + i)) {
            throw std::invalid_argument("invalid RFC3339 timestamp: " + text);
        }
        value = value * 10 + (text[pos + i] - '0');
    }
    return value;
}

} // anonymous

std::chrono::system_clock::time_point ParseRfc3339Utc(const std::string& text) {
    if (text.size() < 20 ||
        text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':')
    {
        throw std::invalid_argument("invalid RFC3339 timestamp: " + text);
    }

    const int year = ParseDigits(text, 0, 4);
    const int month = ParseDigits(text, 5, 2);
    const int day = ParseDigits(text, 8, 2);
    const int hour = ParseDigits(text, 11, 2);
    const int minute = ParseDigits(text, 14, 2);
    const int second = ParseDigits(text, 17, 2);

    size_t pos = 19;
    if (text[pos] == '.') {
        ++pos;
        if (pos >= text.size() || !IsDigitAt(text, pos)) {
            throw std::invalid_argument("invalid RFC3339 fractional seconds: " + text);
        }
        while (pos < text.size() && IsDigitAt(text, pos)) {
            ++pos;
        }
    }
    if (pos + 1 != text.size() || text[pos] != 'Z') {
        throw std::invalid_argument("RFC3339 timestamp must be UTC (Z): " + text);
    }

    if (month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 ||
        second < 0 || second > 59)
    {
        throw std::invalid_argument("invalid RFC3339 timestamp: " + text);
    }

    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;
#if defined(_GNU_SOURCE) || defined(__linux__)
    const time_t secs = timegm(&tm);
#else
    const time_t secs = timegm(&tm);
#endif
    std::tm roundTrip{};
#if defined(_WIN32)
    gmtime_s(&roundTrip, &secs);
#else
    gmtime_r(&secs, &roundTrip);
#endif
    if (roundTrip.tm_year != year - 1900 ||
        roundTrip.tm_mon != month - 1 ||
        roundTrip.tm_mday != day ||
        roundTrip.tm_hour != hour ||
        roundTrip.tm_min != minute ||
        roundTrip.tm_sec != second)
    {
        throw std::invalid_argument("invalid RFC3339 calendar date: " + text);
    }
    return std::chrono::system_clock::from_time_t(secs);
}

std::string FormatRfc3339Utc(std::chrono::system_clock::time_point tp) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

} // namespace NTpcc
