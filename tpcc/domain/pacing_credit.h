#pragma once

#include <chrono>
#include <algorithm>

namespace NTpcc {

// Cap accumulated sleep credit so a stuck timer cannot collapse pacing to zero.
// Two New-Order keying times (18s) covers large transient scheduler overshoot.
inline constexpr std::chrono::milliseconds DefaultPacingCreditCap{36000};

// Reduce the next requested keying/think sleep by outstanding credit.
inline std::chrono::milliseconds ApplyPacingCredit(
    std::chrono::milliseconds& credit,
    std::chrono::milliseconds requested)
{
    if (requested.count() <= 0) {
        return std::chrono::milliseconds{0};
    }
    if (credit.count() <= 0) {
        return requested;
    }
    const auto use = std::min(credit, requested);
    credit -= use;
    return requested - use;
}

// After a sleep of `requestedSleep` that started at `sleepStart`, accrue any
// wall-clock overshoot into credit (capped).
template <typename TClock>
inline void AccruePacingOvershoot(
    std::chrono::milliseconds& credit,
    std::chrono::milliseconds maxCredit,
    typename TClock::time_point sleepStart,
    std::chrono::milliseconds requestedSleep,
    typename TClock::time_point now = TClock::now())
{
    if (requestedSleep.count() < 0) {
        requestedSleep = std::chrono::milliseconds{0};
    }
    const auto actual = std::chrono::duration_cast<std::chrono::milliseconds>(now - sleepStart);
    if (actual <= requestedSleep) {
        return;
    }
    credit += (actual - requestedSleep);
    if (credit > maxCredit) {
        credit = maxCredit;
    }
}

} // namespace NTpcc
