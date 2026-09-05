#include <gtest/gtest.h>
#include "versions_json.h"

#include <filesystem>

namespace {
    std::filesystem::path fixture(const char* name) {
        return std::filesystem::path(CK3ACCEL_TEST_FIXTURE_DIR) / name;
    }
}

TEST(VersionsJsonTest, SampleParses) {
    auto entries = ck3accel::load_versions_json(fixture("versions_sample.json"));
    ASSERT_TRUE(entries.has_value());
    ASSERT_EQ(entries->size(), 2u);
    EXPECT_EQ((*entries)[0].version, "1.19.0.6");
    EXPECT_TRUE((*entries)[0].tested);
    EXPECT_TRUE((*entries)[0].auto_disable.empty());
    EXPECT_EQ((*entries)[1].version, "1.19.0.7");
    EXPECT_FALSE((*entries)[1].tested);
    ASSERT_EQ((*entries)[1].auto_disable.size(), 1u);
    EXPECT_EQ((*entries)[1].auto_disable[0], "accel_ai_tick");
}

TEST(VersionsJsonTest, FindMatchExact) {
    auto entries = ck3accel::load_versions_json(fixture("versions_sample.json"));
    ASSERT_TRUE(entries.has_value());
    auto* m = ck3accel::find_match(
        *entries, 1738329600,
        "aaaa1111bbbb2222cccc3333dddd4444eeee5555ffff66667777888899990000");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->version, "1.19.0.6");
}

TEST(VersionsJsonTest, FindMatchMiss) {
    auto entries = ck3accel::load_versions_json(fixture("versions_sample.json"));
    ASSERT_TRUE(entries.has_value());
    auto* m = ck3accel::find_match(*entries, 1, "0000");
    EXPECT_EQ(m, nullptr);
}

TEST(VersionsJsonTest, MissingFileReturnsNullopt) {
    auto entries = ck3accel::load_versions_json(fixture("does_not_exist.json"));
    EXPECT_FALSE(entries.has_value());
}
