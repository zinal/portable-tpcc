#pragma once

#include <rng.h>

#include <string>

namespace NTpcc::NGenerator {

inline std::string RandomAString(TSeededRng& rng, int length, char baseChar = 'a') {
    if (length <= 0) {
        return {};
    }
    std::string result;
    result.reserve(static_cast<size_t>(length));
    for (int i = 0; i < length; ++i) {
        result += static_cast<char>(baseChar + static_cast<int>(RandomNumber(rng, 0, 25)));
    }
    return result;
}

inline std::string RandomAString(TSeededRng& rng, int minLength, int maxLength, char baseChar = 'a') {
    int length = static_cast<int>(RandomNumber(rng, minLength, maxLength));
    return RandomAString(rng, length, baseChar);
}

inline std::string RandomNumericString(TSeededRng& rng, int length) {
    std::string result;
    result.reserve(static_cast<size_t>(length));
    for (int i = 0; i < length; ++i) {
        result += static_cast<char>('0' + static_cast<int>(RandomNumber(rng, 0, 9)));
    }
    return result;
}

// Inserts "ORIGINAL" into a random position of an a-string of length [minLen, maxLen].
inline std::string RandomDataWithOriginalChance(TSeededRng& rng, int minLen, int maxLen, int originalPercent) {
    int len = static_cast<int>(RandomNumber(rng, minLen, maxLen));
    int randPct = static_cast<int>(RandomNumber(rng, 1, 100));
    if (randPct > originalPercent) {
        return RandomAString(rng, len);
    }
    int startOrig = static_cast<int>(RandomNumber(rng, 0, len - 8));
    return RandomAString(rng, startOrig) + "ORIGINAL" + RandomAString(rng, len - startOrig - 8);
}

} // namespace NTpcc::NGenerator
