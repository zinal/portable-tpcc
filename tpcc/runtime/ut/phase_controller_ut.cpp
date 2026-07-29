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
    EXPECT_TRUE(ctl.MayAdmit());
    EXPECT_FALSE(ctl.MayRecord());

    ctl.Tick(schedule.MeasurementStart);
    EXPECT_EQ(ctl.Phase(), ERunPhase::Measure);
    EXPECT_TRUE(ctl.MayAdmit());
    EXPECT_TRUE(ctl.MayRecord());

    ctl.Tick(schedule.MeasurementEnd);
    EXPECT_EQ(ctl.Phase(), ERunPhase::Drain);
    EXPECT_FALSE(ctl.MayAdmit());
    EXPECT_FALSE(ctl.MayRecord());
}
