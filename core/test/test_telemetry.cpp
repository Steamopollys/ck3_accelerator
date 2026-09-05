#include <gtest/gtest.h>
#include "telemetry.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// unique temp path under system temp; removed before use.
fs::path temp_csv(const char* stem) {
    fs::path p = fs::temp_directory_path() / (std::string("ck3accel_test_") + stem + ".csv");
    std::error_code ec;
    fs::remove(p, ec);
    return p;
}

std::vector<std::string> read_lines(const fs::path& p) {
    std::vector<std::string> lines;
    std::ifstream in(p, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

} // namespace

TEST(TelemetryTest, EnabledRoundTrip) {
    fs::path csv = temp_csv("enabled");

    ck3accel::telemetry_init(/*enabled=*/true, csv, "sess-001", "1.19.0.6");
    ck3accel::telemetry_report("accel_save_load.decompress_ms", 12.5);
    ck3accel::telemetry_report("accel_save_load.decompress_ms", 7.25);
    ck3accel::telemetry_flush();

    ASSERT_TRUE(fs::exists(csv)) << "telemetry file not created: " << csv;

    std::vector<std::string> lines = read_lines(csv);
    ASSERT_EQ(lines.size(), 3u) << "expected header + 2 rows";
    EXPECT_EQ(lines[0], "timestamp_iso8601,session_id,game_version,metric,value");

    // row: <iso8601>,sess-001,1.19.0.6,accel_save_load.decompress_ms,<value>
    EXPECT_NE(lines[1].find(",sess-001,1.19.0.6,accel_save_load.decompress_ms,12.5"),
              std::string::npos) << lines[1];
    EXPECT_NE(lines[2].find(",sess-001,1.19.0.6,accel_save_load.decompress_ms,7.25"),
              std::string::npos) << lines[2];

    std::error_code ec;
    fs::remove(csv, ec);
}

TEST(TelemetryTest, DisabledWritesNoFile) {
    fs::path csv = temp_csv("disabled");

    ck3accel::telemetry_init(/*enabled=*/false, csv, "sess-002", "1.19.0.6");
    ck3accel::telemetry_report("anything", 1.0);
    ck3accel::telemetry_flush();

    EXPECT_FALSE(fs::exists(csv)) << "telemetry file created while disabled: " << csv;

    std::error_code ec;
    fs::remove(csv, ec);
}
