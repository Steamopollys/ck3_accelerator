#include "plugin_loader.h"

#include "crash_sentinel.h"
#include "hook_engine.h"
#include "kill_switch.h"
#include "logger.h"
#include "paths.h"
#include "pattern_scanner.h"
#include "tick_epoch.h"
#include "plugin_gate.h"
#include "telemetry.h"
#include "version_detect.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ck3accel {

namespace {

// tombstone filename (also in core_init.cpp).
constexpr const char* kTombstoneName = "ck3accel_safe_mode.tombstone";

// cached VersionInfo, set by load_plugins before build_core_api runs, so
// core_api_game_version mirrors init-time detection (no re-hash).
VersionInfo g_cached_vi;

// ---- log shim ---------------------------------------------------------------
// CoreApi.log is C-ABI: void(int level, const char*). map int -> LogLevel
// (clamped) and forward to ck3accel::log.
void core_api_log(int level, const char* message) {
    LogLevel lvl;
    switch (level) {
        case 0:  lvl = LogLevel::Trace;    break;
        case 1:  lvl = LogLevel::Debug;    break;
        case 2:  lvl = LogLevel::Info;     break;
        case 3:  lvl = LogLevel::Warn;     break;
        case 4:  lvl = LogLevel::Error;    break;
        case 5:  lvl = LogLevel::Critical; break;
        default: lvl = LogLevel::Info;     break;
    }
    log(lvl, message ? std::string_view{message} : std::string_view{});
}

// ---- game_version backing storage ------------------------------------------
// VersionInfo_C holds raw pointers, so its strings and char* array must outlive
// every plugin call. keep them in function-local statics built once from g_cached_vi.
const VersionInfo_C* core_api_game_version() {
    static VersionInfo              s_vi           = g_cached_vi;
    static std::string              s_version      = s_vi.version;
    static std::string              s_sha          = s_vi.text_sha256;
    static std::vector<const char*> s_disable_ptrs;
    static VersionInfo_C s_c = [&] {
        s_disable_ptrs.reserve(s_vi.auto_disable.size());
        for (const auto& name : s_vi.auto_disable) {
            s_disable_ptrs.push_back(name.c_str());
        }
        VersionInfo_C c{};
        switch (s_vi.status) {
            case DetectionStatus::KnownTested:   c.status = 0; break;
            case DetectionStatus::KnownUntested: c.status = 1; break;
            case DetectionStatus::Unknown:       c.status = 2; break;
        }
        c.version            = s_version.c_str();
        c.pe_timestamp       = s_vi.pe_timestamp;
        c.text_sha256        = s_sha.c_str();
        c.auto_disable       = s_disable_ptrs.empty() ? nullptr : s_disable_ptrs.data();
        c.auto_disable_count = static_cast<int>(s_disable_ptrs.size());
        return c;
    }();
    return &s_c;
}

// ---- kill-switch -----------------------------------------------------------
int core_api_is_kill_switch_active() {
    return is_kill_switch_active();
}

// ---- report_metric: telemetry + first-call stability anchor ----------------
void core_api_report_metric(const char* name, double value) {
    static std::atomic<bool> s_first_done{false};
    bool expected = false;
    if (s_first_done.compare_exchange_strong(expected, true)) {
        // first metric a plugin emits = first successful save-load. we did real
        // work: clear the crash tombstone.
        crash_sentinel_mark_stable();
    }
    telemetry_report(name, value);
}

// ---- scan -> scan_text -----------------------------------------------------
void* core_api_scan(const char* signature) {
    if (!signature) return nullptr;
    ScanResult r = scan_text(signature);
    if (r.status != ScanStatus::Found) return nullptr;
    return const_cast<void*>(static_cast<const void*>(r.address));
}

// ---- install_hook -> hook_engine ------------------------------------------
HookHandle* core_api_install_hook(HookSetId set, void* target,
                                  void* detour, void** trampoline_out) {
    return install_hook(set, target, detour, trampoline_out);
}

// ---- shared tick-epoch service --------------------------------------------
int      core_api_ensure_tick_epoch() { return tick_epoch_ensure() ? 1 : 0; }
uint32_t core_api_tick_epoch()        { return tick_epoch_get(); }
int      core_api_in_tick()           { return tick_epoch_in_tick(); }
void     core_api_bump_epoch()        { tick_epoch_bump(); }

} // namespace

const CoreApi* build_core_api() {
    static CoreApi api = [] {
        CoreApi a{};
        a.struct_size           = static_cast<uint32_t>(sizeof(CoreApi));
        a.abi_version           = CK3ACCEL_ABI_VERSION;
        a.log                   = &core_api_log;
        a.game_version          = &core_api_game_version;
        a.is_kill_switch_active = &core_api_is_kill_switch_active;
        a.report_metric         = &core_api_report_metric;
        a.scan                  = &core_api_scan;
        a.install_hook          = &core_api_install_hook;
        a.ensure_tick_epoch     = &core_api_ensure_tick_epoch;
        a.tick_epoch            = &core_api_tick_epoch;
        a.in_tick               = &core_api_in_tick;
        a.bump_epoch            = &core_api_bump_epoch;
        return a;
    }();
    return &api;
}

std::vector<std::string> filter_allowlisted(
    const std::vector<std::string>& dll_stems,
    const std::unordered_map<std::string, bool>& allowlist) {
    std::vector<std::string> kept;
    for (const auto& stem : dll_stems) {
        auto it = allowlist.find(stem);
        if (it != allowlist.end() && it->second) {
            kept.push_back(stem);
        }
    }
    return kept;
}

void load_plugins(const Config& cfg, const VersionInfo& vi, std::uint32_t session_mode) {
    namespace fs = std::filesystem;

    g_cached_vi = vi;  // feed build_core_api's game_version mirror (no re-hash)

    // prime core_api_game_version()'s function-local statics HERE, on the loader
    // thread, right after writing g_cached_vi. this happens-before the one-time
    // static init, so a later game-thread call gets a fully built object instead
    // of racing the init.
    core_api_game_version();

    const fs::path plugins_dir    = install_directory() / "plugins";
    const fs::path tombstone_path = install_directory() / kTombstoneName;

    // discover plugin DLL stems in install/plugins.
    std::vector<std::string> stems;
    std::error_code ec;
    if (fs::is_directory(plugins_dir, ec)) {
        for (const auto& entry : fs::directory_iterator(plugins_dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const fs::path& p = entry.path();
            if (p.extension() == ".dll") {
                stems.push_back(p.stem().string());
            }
        }
    }
    if (ec) {
        LOG_WARN("plugin discovery failed enumerating " + plugins_dir.string());
    }

    std::vector<std::string> allowlisted = filter_allowlisted(stems, cfg.plugins);
    {
        std::ostringstream os;
        os << "plugin loader: " << stems.size() << " dll(s) discovered, "
           << allowlisted.size() << " allowlisted; session_mode=0x"
           << std::hex << session_mode;
        LOG_INFO(os.str());
    }

    // safe-mode skip-set: a surviving tombstone from a prior launch boots the
    // plugin it names disabled this launch (per-plugin safe-mode), then clears the
    // tombstone so we don't loop. an EMPTY arming_plugin means the prior session
    // just ended before the stability window (e.g. a quick quit) with no plugin
    // implicated: not a crash, so clear it quietly rather than "disabling" the
    // empty-named plugin.
    std::string safe_mode_skip;
    if (auto tomb = read_tombstone(tombstone_path)) {
        if (!tomb->arming_plugin.empty()) {
            safe_mode_skip = tomb->arming_plugin;
            LOG_WARN("crash tombstone from a previous session implicates plugin \""
                     + safe_mode_skip + "\"; it will boot DISABLED (per-plugin safe-mode)");
        } else {
            LOG_INFO("previous session ended before the stability window; "
                     "no plugin implicated, clearing crash tombstone");
        }
        clear_tombstone(tombstone_path);
    }

    const CoreApi* host = build_core_api();

    SessionContext ctx;
    ctx.game_version = vi.version;       // may be empty when status==Unknown
    ctx.session_mode = session_mode;

    for (const std::string& stem : allowlisted) {
        const fs::path dll_path = plugins_dir / (stem + ".dll");

        HMODULE mod = ::LoadLibraryW(dll_path.wstring().c_str());
        if (!mod) {
            LOG_WARN("LoadLibrary failed for " + dll_path.string());
            continue;
        }

        auto query = reinterpret_cast<CK3Accel_Query_t>(
            reinterpret_cast<void*>(::GetProcAddress(mod, "CK3Accel_Query")));
        auto init = reinterpret_cast<CK3Accel_Init_t>(
            reinterpret_cast<void*>(::GetProcAddress(mod, "CK3Accel_Init")));
        if (!query || !init) {
            LOG_WARN("plugin " + stem + " missing CK3Accel_Query/CK3Accel_Init export");
            ::FreeLibrary(mod);
            continue;
        }

        const CK3AccelPluginInfo* info = query(CK3ACCEL_ABI_VERSION);
        if (!info) {
            LOG_WARN("plugin " + stem + " CK3Accel_Query returned null");
            ::FreeLibrary(mod);
            continue;
        }

        // per-plugin safe-mode: skip the offender named by a surviving tombstone.
        if (!safe_mode_skip.empty() && safe_mode_skip == stem) {
            LOG_WARN("plugin " + stem + " skipped: per-plugin safe-mode (prior crash)");
            ::FreeLibrary(mod);
            continue;
        }

        // allowlisted == name true in [plugins] (always true here). auto_disabled
        // == name in this build's auto_disable list.
        const bool allowlisted_flag = true;
        bool auto_disabled = false;
        for (const std::string& bad : vi.auto_disable) {
            if (bad == stem) { auto_disabled = true; break; }
        }

        GateDecision decision = evaluate_plugin(
            *info, CK3ACCEL_ABI_VERSION, ctx, allowlisted_flag, auto_disabled);
        if (decision != GateDecision::Accept) {
            LOG_WARN("plugin " + stem + " rejected by gate: "
                     + gate_decision_str(decision));
            ::FreeLibrary(mod);
            continue;
        }

        HookSetId set = register_hook_set(stem);
        if (set == kInvalidHookSet) {
            LOG_WARN("plugin " + stem + " could not be assigned a hook set");
            ::FreeLibrary(mod);
            continue;
        }

        // record the arming plugin BEFORE its Init installs hooks so an in-range
        // crash during arming is attributed to it. also register the module's
        // address range so a fault deep in its detour body (past the 64-byte entry
        // span) is still attributed.
        crash_sentinel_set_arming_plugin(stem);
        crash_sentinel_register_module(stem, reinterpret_cast<const void*>(mod));

        CK3AccelRegistrar reg{};
        reg.struct_size = static_cast<uint32_t>(sizeof(CK3AccelRegistrar));
        reg.hook_set    = set;

        int rc = init(host, &reg);
        if (rc != 0) {
            LOG_WARN("plugin " + stem + " CK3Accel_Init returned " + std::to_string(rc)
                     + "; removing its hook set");
            remove_set(set);
            ::FreeLibrary(mod);
            continue;
        }

        LOG_INFO("plugin loaded: " + stem);
    }

    crash_sentinel_set_arming_plugin("");  // no plugin currently arming
    LOG_INFO("plugin loader: load sequence complete");
}

} // namespace ck3accel
