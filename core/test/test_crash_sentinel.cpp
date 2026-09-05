#include <gtest/gtest.h>
#include "crash_sentinel.h"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {

// unique tombstone path per test under system temp; removed on dtor.
class ScopedTombstone {
public:
    explicit ScopedTombstone(const char* leaf)
        : path_(fs::temp_directory_path() /
                (std::string("ck3accel_tombstone_test_") + leaf + ".txt")) {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    ~ScopedTombstone() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

} // namespace

TEST(CrashSentinelTest, WriteReadRoundTrip) {
    ScopedTombstone t("roundtrip");

    ck3accel::Tombstone in;
    in.game_version  = "1.19.0.6";
    in.timestamp_utc = "2026-05-29T12:34:56Z";
    in.arming_plugin = "accel_save_load";

    ck3accel::write_tombstone(t.path(), in);
    ASSERT_TRUE(fs::exists(t.path()));

    auto out = ck3accel::read_tombstone(t.path());
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->game_version, "1.19.0.6");
    EXPECT_EQ(out->timestamp_utc, "2026-05-29T12:34:56Z");
    EXPECT_EQ(out->arming_plugin, "accel_save_load");
}

TEST(CrashSentinelTest, WriteThenClearLeavesNoTombstone) {
    ScopedTombstone t("clear");

    ck3accel::Tombstone in;
    in.game_version  = "1.19.0.6";
    in.timestamp_utc = "2026-05-29T12:34:56Z";
    in.arming_plugin = "accel_save_load";

    ck3accel::write_tombstone(t.path(), in);
    ASSERT_TRUE(fs::exists(t.path()));

    ck3accel::clear_tombstone(t.path());
    EXPECT_FALSE(fs::exists(t.path()));

    auto out = ck3accel::read_tombstone(t.path());
    EXPECT_FALSE(out.has_value());
}

TEST(CrashSentinelTest, MissingFileReturnsNullopt) {
    ScopedTombstone t("missing");
    // Never written.
    auto out = ck3accel::read_tombstone(t.path());
    EXPECT_FALSE(out.has_value());
}

TEST(CrashSentinelTest, MalformedFileReturnsNullopt) {
    ScopedTombstone t("malformed");

    {
        std::ofstream os(t.path(), std::ios::binary | std::ios::trunc);
        // no recognized keys, no '=': structurally invalid.
        os << "this is not a tombstone\n";
        os << "garbage without an equals sign\n";
    }
    ASSERT_TRUE(fs::exists(t.path()));

    auto out = ck3accel::read_tombstone(t.path());
    EXPECT_FALSE(out.has_value());
}

TEST(CrashSentinelTest, EmptyArmingPluginRoundTrips) {
    ScopedTombstone t("empty_plugin");

    ck3accel::Tombstone in;
    in.game_version  = "1.19.0.6";
    in.timestamp_utc = "2026-05-29T00:00:00Z";
    in.arming_plugin = "";  // initial state at install, before any plugin arms

    ck3accel::write_tombstone(t.path(), in);
    auto out = ck3accel::read_tombstone(t.path());
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->game_version, "1.19.0.6");
    EXPECT_EQ(out->timestamp_utc, "2026-05-29T00:00:00Z");
    EXPECT_TRUE(out->arming_plugin.empty());
}
