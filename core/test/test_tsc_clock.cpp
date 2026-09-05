#include <ck3accel/tsc_clock.h>
#include <gtest/gtest.h>

using ck3accel::calibrate_tsc_per_sec;
using ck3accel::tsc_to_ns;

TEST(TscClock, CalibrationDerivesRate) {
    // rate = tsc_delta * qpc_freq / qpc_delta.
    // 0.5 s at 10 MHz QPC => qpc_delta=5,000,000; TSC +1.5e9 => 3 GHz.
    EXPECT_EQ(calibrate_tsc_per_sec(1500000000ull, 5000000ull, 10000000ull),
              3000000000ull);
}

TEST(TscClock, CalibrationZeroGuards) {
    EXPECT_EQ(calibrate_tsc_per_sec(123, 0, 10000000ull), 0u);   // no wall time
    EXPECT_EQ(calibrate_tsc_per_sec(123, 5000000ull, 0), 0u);    // no qpc freq
}

TEST(TscClock, TscToNsConverts) {
    // 3e9 tsc/sec => 3 ticks per ns => 3000 ticks = 1000 ns.
    EXPECT_EQ(tsc_to_ns(3000ull, 3000000000ull), 1000ull);
    EXPECT_EQ(tsc_to_ns(0ull, 3000000000ull), 0ull);
    EXPECT_EQ(tsc_to_ns(5ull, 0ull), 0ull);                      // unknown rate
}
