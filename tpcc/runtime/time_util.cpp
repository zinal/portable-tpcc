#include "time_util.h"

#include <cstdio>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace NTpcc {

std::chrono::system_clock::time_point ParseRfc3339Utc(const std::string& text) {
    if (text.size() < 20) {
        throw std::invalid_argument("RFC3339 timestamp too short: " + text);
    }

    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    char tz = 0;
    // Accept optional fractional seconds before Z.
    const int n = std::sscanf(
        text.c_str(), "%d-%d-%dT%d:%d:%d%c",
        &year, &month, &day, &hour, &minute, &second, &tz);
    if (n < 6) {
        throw std::invalid_argument("invalid RFC3339 timestamp: " + text);
    }
    if (n >= 7 && tz != 'Z' && tz != '.' && tz != '+' && tz != '-') {
        throw std::invalid_argument("RFC3339 timestamp must be UTC (Z): " + text);
    }
    if (!text.empty() && text.back() != 'Z') {
        // Allow "...Z" only for orchestrated start-at (normative UTC).
        if (text.find('Z') == std::string::npos) {
            throw std::invalid_argument("RFC3339 timestamp must end with Z: " + text);
        }
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
    if (secs == static_cast<time_t>(-1)) {
        throw std::invalid_argument("failed to convert RFC3339 timestamp: " + text);
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
