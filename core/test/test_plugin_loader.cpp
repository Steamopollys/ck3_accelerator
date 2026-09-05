#include <gtest/gtest.h>
#include "plugin_loader.h"

#include <string>
#include <unordered_map>
#include <vector>

TEST(PluginLoaderTest, FilterKeepsAllowlistedTrueInOrder) {
    std::vector<std::string> stems{"accel_save_load", "sample_noop", "other"};
    std::unordered_map<std::string, bool> allow{
        {"accel_save_load", true},
        {"sample_noop", true},
        {"other", false},
    };
    auto kept = ck3accel::filter_allowlisted(stems, allow);
    ASSERT_EQ(kept.size(), 2u);
    EXPECT_EQ(kept[0], "accel_save_load");  // order preserved
    EXPECT_EQ(kept[1], "sample_noop");
}

TEST(PluginLoaderTest, FilterDropsFalseEntries) {
    std::vector<std::string> stems{"a", "b"};
    std::unordered_map<std::string, bool> allow{
        {"a", false},
        {"b", false},
    };
    auto kept = ck3accel::filter_allowlisted(stems, allow);
    EXPECT_TRUE(kept.empty());
}

TEST(PluginLoaderTest, FilterDropsAbsentEntries) {
    std::vector<std::string> stems{"present", "missing"};
    std::unordered_map<std::string, bool> allow{
        {"present", true},
        // "missing" not in allowlist -> dropped
    };
    auto kept = ck3accel::filter_allowlisted(stems, allow);
    ASSERT_EQ(kept.size(), 1u);
    EXPECT_EQ(kept[0], "present");
}

TEST(PluginLoaderTest, FilterEmptyStemsYieldsEmpty) {
    std::vector<std::string> stems{};
    std::unordered_map<std::string, bool> allow{{"a", true}};
    auto kept = ck3accel::filter_allowlisted(stems, allow);
    EXPECT_TRUE(kept.empty());
}
