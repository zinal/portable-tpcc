#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace NTpcc {

enum class ERunPhase : int {
    Prepare = 0,
    Ramp = 1,
    Measure = 2,
    Drain = 3,
    Stop = 4,
};

struct TPhaseDurations {
    int64_t RampUpMs = 0;
    int64_t MeasurementMs = 0;
    int64_t TransactionDrainMs = 0;
    int64_t StopGraceMs = 0;
};

struct TPhaseSchedule {
    std::chrono::system_clock::time_point RampStart{};
    std::chrono::system_clock::time_point MeasurementStart{};
    std::chrono::system_clock::time_point MeasurementEnd{};
    std::chrono::system_clock::time_point DrainDeadline{};
    std::chrono::system_clock::time_point StopGraceDeadline{};
};

inline TPhaseSchedule BuildPhaseSchedule(
    std::chrono::system_clock::time_point rampStart,
    const TPhaseDurations& d)
{
    using namespace std::chrono;
    TPhaseSchedule s;
    s.RampStart = rampStart;
    s.MeasurementStart = rampStart + milliseconds(d.RampUpMs);
    s.MeasurementEnd = s.MeasurementStart + milliseconds(d.MeasurementMs);
    s.DrainDeadline = s.MeasurementEnd + milliseconds(d.TransactionDrainMs);
    s.StopGraceDeadline = s.DrainDeadline + milliseconds(d.StopGraceMs);
    return s;
}

// Shared by runner (writer) and terminals (readers).
class TPhaseController {
public:
    TPhaseController() = default;

    explicit TPhaseController(TPhaseSchedule schedule)
        : Schedule_(std::move(schedule))
    {}

    void SetSchedule(TPhaseSchedule schedule) {
        Schedule_ = std::move(schedule);
    }

    const TPhaseSchedule& Schedule() const {
        return Schedule_;
    }

    void SetPhase(ERunPhase phase) {
        Phase_.store(static_cast<int>(phase), std::memory_order_release);
    }

    ERunPhase Phase() const {
        return static_cast<ERunPhase>(Phase_.load(std::memory_order_acquire));
    }

    // Wall-clock point inside the half-open measurement interval
    // [MeasurementStart, MeasurementEnd).
    bool InMeasurementInterval(std::chrono::system_clock::time_point t) const {
        return t >= Schedule_.MeasurementStart && t < Schedule_.MeasurementEnd;
    }

    // TPC-C §5.4.2: count only when response-time start and end both lie
    // within the closed measurement interval [MeasurementStart, MeasurementEnd].
    bool CompletelyWithinMeasurement(
        std::chrono::system_clock::time_point start,
        std::chrono::system_clock::time_point end) const
    {
        return start >= Schedule_.MeasurementStart
            && end <= Schedule_.MeasurementEnd
            && end >= start;
    }

    // New business transactions may start only in Ramp or Measure, with an
    // absolute cutoff at MeasurementEnd so Tick() lag cannot admit late work.
    bool MayAdmit() const {
        const auto p = Phase();
        if (p != ERunPhase::Ramp && p != ERunPhase::Measure) {
            return false;
        }
        return std::chrono::system_clock::now() < Schedule_.MeasurementEnd;
    }

    // Measurement metrics are recorded only inside the measurement interval.
    // Uses absolute schedule timestamps (not the Tick()-published phase enum).
    bool MayRecord() const {
        return InMeasurementInterval(std::chrono::system_clock::now());
    }

    // Advance phase from wall-clock now (monotonic transitions only).
    void Tick(std::chrono::system_clock::time_point now) {
        const auto cur = Phase();
        if (cur == ERunPhase::Stop || cur == ERunPhase::Prepare) {
            return;
        }
        if (now >= Schedule_.MeasurementEnd) {
            if (cur != ERunPhase::Drain && cur != ERunPhase::Stop) {
                SetPhase(ERunPhase::Drain);
            }
            return;
        }
        if (now >= Schedule_.MeasurementStart) {
            if (cur == ERunPhase::Ramp) {
                SetPhase(ERunPhase::Measure);
            }
            return;
        }
        if (cur == ERunPhase::Prepare) {
            SetPhase(ERunPhase::Ramp);
        }
    }

private:
    TPhaseSchedule Schedule_;
    std::atomic<int> Phase_{static_cast<int>(ERunPhase::Prepare)};
};

} // namespace NTpcc
