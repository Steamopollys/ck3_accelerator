#include "config.h"

#include <toml++/toml.hpp>

namespace ck3accel {

namespace {
    std::optional<LogLevel> parse_log_level(std::string_view s) {
        if (s == "trace")    return LogLevel::Trace;
        if (s == "debug")    return LogLevel::Debug;
        if (s == "info")     return LogLevel::Info;
        if (s == "warn")     return LogLevel::Warn;
        if (s == "error")    return LogLevel::Error;
        if (s == "critical") return LogLevel::Critical;
        return std::nullopt;
    }
}

Config default_config() {
    return Config{};
}

std::optional<Config> load_config(const std::filesystem::path& path) {
    toml::table tbl;
    try {
        tbl = toml::parse_file(path.string());
    } catch (const toml::parse_error&) {
        return std::nullopt;
    }

    Config c;

    if (auto core = tbl["core"].as_table()) {
        if (auto lvl = (*core)["log_level"].value<std::string>()) {
            auto parsed = parse_log_level(*lvl);
            if (!parsed) return std::nullopt;
            c.core.log_level = *parsed;
        }
        if (auto au = (*core)["allow_untested_versions"].value<bool>()) {
            c.core.allow_untested_versions = *au;
        }
        if (auto ks = (*core)["kill_switch"].value<std::string>()) {
            c.core.kill_switch = *ks;
        }
        if (auto tel = (*core)["telemetry"].value<bool>()) {
            c.core.telemetry = *tel;
        }
        if (auto con = (*core)["console"].value<bool>()) {
            c.core.console = *con;
        }
    }

    if (auto plugins = tbl["plugins"].as_table()) {
        for (auto&& [k, v] : *plugins) {
            if (auto b = v.value<bool>()) {
                c.plugins.emplace(std::string{k.str()}, *b);
            } else {
                return std::nullopt;
            }
        }
    }

    return c;
}

} // namespace ck3accel
