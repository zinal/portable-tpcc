#include "dummy_delay.h"

#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace NTpcc {

namespace {

int64_t ParseNonNegativeInt64(std::string_view text, const std::string& key) {
    if (text.empty()) {
        throw std::runtime_error(key + " must be a non-negative integer");
    }
    size_t idx = 0;
    if (text[0] == '+') {
        idx = 1;
    }
    if (idx >= text.size()) {
        throw std::runtime_error(key + " must be a non-negative integer");
    }
    int64_t value = 0;
    for (; idx < text.size(); ++idx) {
        const char c = text[idx];
        if (c < '0' || c > '9') {
            throw std::runtime_error(key + " must be a non-negative integer");
        }
        const int digit = c - '0';
        if (value > (std::numeric_limits<int64_t>::max() - digit) / 10) {
            throw std::runtime_error(key + " is too large");
        }
        value = value * 10 + digit;
    }
    return value;
}

} // anonymous

TDummyDelayConfig ParseDummyConnection(const std::string& connection) {
    TDummyDelayConfig config;
    bool hasMin = false;
    bool hasMax = false;
    std::istringstream in(connection);
    std::string token;
    while (in >> token) {
        const auto eq = token.find('=');
        if (eq == std::string::npos || eq == 0) {
            throw std::runtime_error(
                "dummy connection must be key=value pairs (delay_us_min, delay_us_max)");
        }
        const std::string key = token.substr(0, eq);
        const std::string value = token.substr(eq + 1);
        if (key == "delay_us_min") {
            config.MinUs = ParseNonNegativeInt64(value, "delay_us_min");
            hasMin = true;
        } else if (key == "delay_us_max") {
            config.MaxUs = ParseNonNegativeInt64(value, "delay_us_max");
            hasMax = true;
        } else {
            throw std::runtime_error("unknown dummy connection key: " + key);
        }
    }
    if (hasMin && !hasMax) {
        config.MaxUs = config.MinUs;
    }
    if (hasMax && !hasMin) {
        config.MinUs = 0;
    }
    ValidateDummyDelayConfig(config);
    return config;
}

void ValidateDummyDelayConfig(const TDummyDelayConfig& config) {
    if (config.MinUs < 0) {
        throw std::runtime_error("delay_us_min must not be negative");
    }
    if (config.MaxUs < 0) {
        throw std::runtime_error("delay_us_max must not be negative");
    }
    if (config.MaxUs < config.MinUs) {
        throw std::runtime_error("delay_us_max must be greater than or equal to delay_us_min");
    }
}

std::chrono::microseconds SampleDummyDelay(const TDummyDelayConfig& config) {
    if (config.MaxUs <= 0) {
        return std::chrono::microseconds{0};
    }
    if (config.MinUs == config.MaxUs) {
        return std::chrono::microseconds{config.MinUs};
    }
    thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int64_t> dist(config.MinUs, config.MaxUs);
    return std::chrono::microseconds{dist(rng)};
}

} // namespace NTpcc
