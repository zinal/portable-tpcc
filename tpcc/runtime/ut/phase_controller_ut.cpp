#include <gtest/gtest.h>

#include <phase_controller.h>

using namespace NTpcc;
using namespace std::chrono;

TEST(PhaseController, ScheduleAndAdmission) {
    const auto ramp = system_clock::time_point{seconds{1000}};
    TPhaseDurations d;
    d.RampUpMs = 5000;
    d.MeasurementMs = 10000;
    d.TransactionDrainMs = 1000;

    auto schedule = BuildPhaseSchedule(ramp, d);
    EXPECT_EQ(schedule.MeasurementStart, ramp + milliseconds(5000));
    EXPECT_EQ(schedule.MeasurementEnd, ramp + milliseconds(15000));
    EXPECT_EQ(schedule.DrainDeadline, ramp + milliseconds(16000));

    TPhaseController ctl(schedule);
    EXPECT_FALSE(ctl.MayAdmit());
    EXPECT_FALSE(ctl.MayRecord());

    ctl.SetPhase(ERunPhase::Ramp);
    // Schedule is in the past relative to wall clock, so absolute MeasurementEnd
    // denies admission even while the published phase is still Ramp.
    EXPECT_FALSE(ctl.MayAdmit());
    EXPECT_FALSE(ctl.MayRecord());

    ctl.Tick(schedule.MeasurementStart);
    EXPECT_EQ(ctl.Phase(), ERunPhase::Measure);
    EXPECT_FALSE(ctl.MayAdmit());
    EXPECT_FALSE(ctl.MayRecord());

    ctl.Tick(schedule.MeasurementEnd);
    EXPECT_EQ(ctl.Phase(), ERunPhase::Drain);
    EXPECT_FALSE(ctl.MayAdmit());
    EXPECT_FALSE(ctl.MayRecord());
}

TEST(PhaseController, CompletelyWithinMeasurement) {
    const auto ramp = system_clock::time_point{seconds{1000}};
    TPhaseDurations d;
    d.RampUpMs = 5000;
    d.MeasurementMs = 10000;
    d.TransactionDrainMs = 1000;

    TPhaseController ctl(BuildPhaseSchedule(ramp, d));
    const auto& s = ctl.Schedule();

    EXPECT_TRUE(ctl.InMeasurementInterval(s.MeasurementStart));
    EXPECT_TRUE(ctl.InMeasurementInterval(s.MeasurementStart + milliseconds(1)));
    EXPECT_FALSE(ctl.InMeasurementInterval(s.MeasurementStart - milliseconds(1)));
    EXPECT_FALSE(ctl.InMeasurementInterval(s.MeasurementEnd));

    EXPECT_TRUE(ctl.CompletelyWithinMeasurement(s.MeasurementStart, s.MeasurementEnd));
    EXPECT_TRUE(ctl.CompletelyWithinMeasurement(
        s.MeasurementStart, s.MeasurementStart + milliseconds(1)));
    EXPECT_TRUE(ctl.CompletelyWithinMeasurement(
        s.MeasurementEnd - milliseconds(1), s.MeasurementEnd));

    // Started before measurement (e.g. inflight wait spanning Ramp → Measure).
    EXPECT_FALSE(ctl.CompletelyWithinMeasurement(
        s.MeasurementStart - milliseconds(1), s.MeasurementStart + milliseconds(10)));

    // Ended after measurement (response time crossed MeasurementEnd).
    EXPECT_FALSE(ctl.CompletelyWithinMeasurement(
        s.MeasurementEnd - milliseconds(10), s.MeasurementEnd + milliseconds(1)));

    EXPECT_FALSE(ctl.CompletelyWithinMeasurement(
        s.MeasurementEnd + milliseconds(1), s.MeasurementEnd + milliseconds(2)));
    EXPECT_FALSE(ctl.CompletelyWithinMeasurement(
        s.MeasurementStart + milliseconds(10), s.MeasurementStart));
}

TEST(PhaseController, AbsoluteAdmitAndRecordIgnoreStalePhase) {
    // Schedule covers "now" so absolute checks can admit/record while Tick lags.
    const auto now = system_clock::now();
    TPhaseDurations d;
    d.RampUpMs = 0;
    d.MeasurementMs = 60'000;
    d.TransactionDrainMs = 1000;

    TPhaseController ctl(BuildPhaseSchedule(now - seconds{1}, d));
    ctl.SetPhase(ERunPhase::Ramp); // stale: Tick has not published Measure yet

    EXPECT_TRUE(ctl.MayAdmit());
    EXPECT_TRUE(ctl.MayRecord());
    EXPECT_TRUE(ctl.InMeasurementInterval(now));
    EXPECT_TRUE(ctl.CompletelyWithinMeasurement(now - milliseconds{100}, now));

    // Past MeasurementEnd: deny even if published phase is still Measure.
    TPhaseController late(BuildPhaseSchedule(now - seconds{120}, d));
    late.SetPhase(ERunPhase::Measure);
    EXPECT_FALSE(late.MayAdmit());
    EXPECT_FALSE(late.MayRecord());
}
