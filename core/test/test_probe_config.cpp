#include <ck3accel/probe_config.h>
#include <gtest/gtest.h>

using ck3accel::parse_eval_probe_config;

TEST(ProbeConfig, DefaultsAllOffWhenEmpty) {
    const auto c = parse_eval_probe_config("");
    EXPECT_FALSE(c.enabled);
    EXPECT_FALSE(c.dispatch);
    EXPECT_FALSE(c.trigger);
    EXPECT_FALSE(c.eventtarget);
    EXPECT_FALSE(c.eventscope);
    EXPECT_FALSE(c.link);
    EXPECT_FALSE(c.freeze);
    EXPECT_EQ(c.freeze_threshold_ms, 150u);
    EXPECT_EQ(c.dump_interval_s, 5u);
}

TEST(ProbeConfig, ParsesBoolsAndIntsAndIgnoresCommentsAndWhitespace) {
    const auto c = parse_eval_probe_config(
        "# the eval probe config\n"
        "eval_probe = true\r\n"
        "probe_freeze=1\n"
        "  probe_dispatch =  yes \n"
        "probe_trigger = false\n"
        "freeze_threshold_ms = 250\n"
        "dump_interval_s=10\n"
        "unknown_key = whatever\n");
    EXPECT_TRUE(c.enabled);
    EXPECT_TRUE(c.freeze);
    EXPECT_TRUE(c.dispatch);
    EXPECT_FALSE(c.trigger);
    EXPECT_EQ(c.freeze_threshold_ms, 250u);
    EXPECT_EQ(c.dump_interval_s, 10u);
}

TEST(ProbeConfig, ZeroDumpIntervalClampedToOne) {
    const auto c = parse_eval_probe_config("dump_interval_s = 0\n");
    EXPECT_EQ(c.dump_interval_s, 1u);  // never 0 (would busy-spin the dump thread)
}
