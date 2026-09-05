#include <gtest/gtest.h>
#include "config.h"

#include <filesystem>

namespace {
    std::filesystem::path fixture(const char* name) {
        // set by CMake at compile time.
        return std::filesystem::path(CK3ACCEL_TEST_FIXTURE_DIR) / name;
    }
}

TEST(ConfigTest, GoodFileParses) {
    auto cfg = ck3accel::load_config(fixture("good_config.toml"));
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->core.log_level, ck3accel::LogLevel::Debug);
    EXPECT_TRUE(cfg->core.allow_untested_versions);
    EXPECT_EQ(cfg->core.kill_switch, "Alt+K");
    EXPECT_TRUE(cfg->core.telemetry);
    EXPECT_EQ(cfg->plugins.size(), 2u);
    EXPECT_TRUE(cfg->plugins.at("save_load"));
    EXPECT_FALSE(cfg->plugins.at("ai_tick"));
}

TEST(ConfigTest, BadFileReturnsNullopt) {
    auto cfg = ck3accel::load_config(fixture("bad_config.toml"));
    EXPECT_FALSE(cfg.has_value());
}

TEST(ConfigTest, MissingFileReturnsNullopt) {
    auto cfg = ck3accel::load_config(fixture("does_not_exist.toml"));
    EXPECT_FALSE(cfg.has_value());
}

TEST(ConfigTest, DefaultConfigIsSensible) {
    auto cfg = ck3accel::default_config();
    EXPECT_EQ(cfg.core.log_level, ck3accel::LogLevel::Info);
    EXPECT_FALSE(cfg.core.allow_untested_versions);
    EXPECT_TRUE(cfg.plugins.empty());
}

TEST(ConfigTest, NewCoreKeysHaveSensibleDefaults) {
    auto cfg = ck3accel::default_config();
    EXPECT_EQ(cfg.core.kill_switch, "Ctrl+Shift+F12");
    EXPECT_FALSE(cfg.core.telemetry);
}
