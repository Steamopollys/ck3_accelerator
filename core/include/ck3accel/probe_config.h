#pragma once
#include <cstdint>
#include <cstdlib>
#include <string>
namespace ck3accel {

struct EvalProbeConfig {
    bool enabled     = false;   // master "eval_probe"
    bool dispatch    = false;   // hook 0x00817C20
    bool trigger     = false;   // hook 0x0334C550
    bool eventtarget = false;   // hook 0x0336AA90
    bool eventscope  = false;   // hook 0x033598C6
    bool link        = false;   // hook 0x0340F790
    bool freeze      = false;   // hook 0x0204EFE0 + watchdog
    std::uint32_t freeze_threshold_ms = 150;
    std::uint32_t dump_interval_s     = 5;
};

namespace detail {
inline std::string trim(const std::string& s) {
    const char* ws = " \t\r\n";
    const auto a = s.find_first_not_of(ws);
    if (a == std::string::npos) return {};
    const auto b = s.find_last_not_of(ws);
    return s.substr(a, b - a + 1);
}
inline bool truthy(const std::string& v) {
    return v == "1" || v == "true" || v == "yes" || v == "on" || v == "TRUE";
}
}  // namespace detail

// parse a key=value config (one per line; '#' starts a comment; unknown keys
// ignored). missing keys keep defaults. dump_interval_s clamped to >=1.
inline EvalProbeConfig parse_eval_probe_config(const std::string& text) {
    EvalProbeConfig c;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t nl = text.find('\n', pos);
        const std::string raw =
            text.substr(pos, (nl == std::string::npos ? text.size() : nl) - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;

        std::string line = raw;
        const auto hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = detail::trim(line.substr(0, eq));
        const std::string val = detail::trim(line.substr(eq + 1));
        if (key.empty()) continue;

        if      (key == "eval_probe")     c.enabled     = detail::truthy(val);
        else if (key == "probe_dispatch") c.dispatch    = detail::truthy(val);
        else if (key == "probe_trigger")  c.trigger     = detail::truthy(val);
        else if (key == "probe_eventtarget") c.eventtarget = detail::truthy(val);
        else if (key == "probe_eventscope")  c.eventscope  = detail::truthy(val);
        else if (key == "probe_link")     c.link        = detail::truthy(val);
        else if (key == "probe_freeze")   c.freeze      = detail::truthy(val);
        else if (key == "freeze_threshold_ms")
            c.freeze_threshold_ms =
                static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
        else if (key == "dump_interval_s")
            c.dump_interval_s =
                static_cast<std::uint32_t>(std::strtoul(val.c_str(), nullptr, 10));
    }
    if (c.dump_interval_s == 0) c.dump_interval_s = 1;
    return c;
}

}  // namespace ck3accel
