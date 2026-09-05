#pragma once
#include <ck3accel/log_level.h>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace ck3accel {

struct CoreConfig {
    LogLevel    log_level = LogLevel::Info;
    bool        allow_untested_versions = false;
    std::string kill_switch = "Ctrl+Shift+F12";   // global panic hotkey
    bool        telemetry = false;                 // local-only metrics; off by default
    bool        console = false;                   // opens a live log console window; debugging only
};

struct Config {
    CoreConfig core{};
    std::unordered_map<std::string, bool> plugins{};
};

// load config. nullopt on any parse failure (bad TOML, unknown enum). caller
// should fall back to default_config() and log a warning.
std::optional<Config> load_config(const std::filesystem::path& path);

// default-constructed Config; used when no file exists.
Config default_config();

} // namespace ck3accel
