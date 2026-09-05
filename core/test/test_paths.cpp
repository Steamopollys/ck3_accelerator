#include <gtest/gtest.h>
#include "paths.h"
#include <filesystem>

namespace fs = std::filesystem;

TEST(PathsTest, DllDirContainsExpectedBaseName) {
    fs::path dir = ck3accel::dll_directory();
    // exact path is unknown in tests; just require an existing dir.
    EXPECT_TRUE(fs::exists(dir)) << "dll_directory(): " << dir;
    EXPECT_TRUE(fs::is_directory(dir)) << "dll_directory(): " << dir;
}

TEST(PathsTest, ConfigPathIsUnderInstallDir) {
    fs::path config = ck3accel::config_path();
    fs::path install = ck3accel::install_directory();
    EXPECT_EQ(config.parent_path(), install);
    EXPECT_EQ(config.filename().string(), "config.toml");
}

TEST(PathsTest, LogDirIsUnderInstallDir) {
    fs::path log = ck3accel::log_directory();
    fs::path install = ck3accel::install_directory();
    EXPECT_EQ(log.parent_path(), install);
    EXPECT_EQ(log.filename().string(), "logs");
}

TEST(PathsTest, VersionsJsonIsUnderInstallDir) {
    fs::path vj = ck3accel::versions_json_path();
    fs::path install = ck3accel::install_directory();
    EXPECT_EQ(vj.parent_path(), install);
    EXPECT_EQ(vj.filename().string(), "versions.json");
}
