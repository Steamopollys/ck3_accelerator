#include "core_init.h"

#include "config.h"
#include "crash_sentinel.h"
#include "hook_engine.h"
#include "kill_switch.h"
#include "logger.h"
#include "paths.h"
#include "plugin_loader.h"
#include "telemetry.h"
#include "version_detect.h"

#include <ck3accel/core_api.h>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <memory>
#include <random>
#include <sstream>
#include <string>

namespace ck3accel {

namespace {

// loader-thread handle; core_shutdown joins it before tearing down kill_switch /
// crash_sentinel state the loader may still be building. null on the Unknown-build
// path (no thread spawned).
std::atomic<HANDLE> g_loader_thread{nullptr};

    const char* status_str(DetectionStatus s) {
        switch (s) {
            case DetectionStatus::KnownTested:   return "KnownTested";
            case DetectionStatus::KnownUntested: return "KnownUntested";
            case DetectionStatus::Unknown:       return "Unknown";
        }
        return "?";
    }

    // tombstone filename (also in plugin_loader.cpp).
    constexpr const char* kTombstoneName = "ck3accel_safe_mode.tombstone";

    // heap payload owned by the loader thread; freed before it returns.
    struct LoaderPayload {
        Config      cfg;
        VersionInfo vi;
    };

    std::string make_session_id() {
        std::random_device rd;
        std::uniform_int_distribution<unsigned> dist(0, 0xFFFFFFFFu);
        std::ostringstream os;
        os << std::hex << std::setw(8) << std::setfill('0') << dist(rd);
        return os.str();
    }

    std::string utc_timestamp_now() {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        gmtime_s(&tm, &t);
        std::ostringstream os;
        os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return os.str();
    }

    // loader thread. spawned from core_init so LoadLibrary runs outside the
    // DllMain loader lock.
    DWORD WINAPI loader_thread_main(LPVOID param) {
        std::unique_ptr<LoaderPayload> payload(static_cast<LoaderPayload*>(param));

        if (!hook_engine_init()) {
            LOG_ERROR("hook engine failed to initialize; no plugins will load");
            return 1;
        }

        // telemetry: off unless config opts in; metrics.csv in log_directory().
        const bool telemetry_on = payload->cfg.core.telemetry;
        telemetry_init(telemetry_on,
                       log_directory() / "metrics.csv",
                       make_session_id(),
                       payload->vi.version);

        // kill-switch: parse hotkey; on press soft-disable every hook set and latch active.
        Hotkey hk = parse_hotkey(payload->cfg.core.kill_switch);
        if (hk.valid) {
            kill_switch_start(hk, [] { disable_all(); });
        } else {
            LOG_WARN("kill-switch hotkey \"" + payload->cfg.core.kill_switch
                     + "\" did not parse; panic hotkey disabled this session");
        }

        // crash sentinel: write tombstone now, arm the fallback-stable timer.
        Tombstone tomb;
        tomb.game_version  = payload->vi.version;
        tomb.timestamp_utc = utc_timestamp_now();
        tomb.arming_plugin = "";  // set per-plugin by load_plugins
        crash_sentinel_install(install_directory() / kTombstoneName,
                               std::move(tomb),
                               /*fallback_seconds=*/120);

        // SP-only (deferred RE; documented limitation).
        load_plugins(payload->cfg, payload->vi, CK3ACCEL_MODE_SP);
        return 0;
    }
}

bool core_init() {
    if (!init_logger()) {
        return false;  // can't log without a logger; bail silently
    }

    LOG_INFO("ck3accel_core attaching");

    // config (non-fatal; fall back to defaults).
    auto cfg_opt = load_config(config_path());
    Config cfg = cfg_opt.value_or(default_config());
    if (!cfg_opt) {
        LOG_WARN("config.toml missing or invalid; using defaults");
    }

    // optional debug console; off by default.
    if (cfg.core.console) {
        enable_console_logging();
        LOG_INFO("debug console attached");
    }

    auto vi = detect_version();
    std::ostringstream os;
    os << "CK3 detection: status=" << status_str(vi.status)
       << " pe_timestamp=0x" << std::hex << vi.pe_timestamp
       << " text_sha256=" << vi.text_sha256
       << " version=\"" << vi.version << "\"";
    LOG_INFO(os.str());

    if (vi.status == DetectionStatus::Unknown) {
        LOG_WARN("Unknown CK3 build. Loading in observe-only mode; no hooks will install.");
        LOG_INFO("ck3accel_core init complete (observe-only; loader not spawned)");
        return true;  // do not arm hooks against an unknown binary
    }
    if (vi.status == DetectionStatus::KnownUntested
        && !cfg.core.allow_untested_versions) {
        LOG_WARN("Known but untested CK3 build; allow_untested_versions=false. "
                 "Risky plugins will be skipped.");
    }

    // spawn the loader thread: LoadLibrary must not run inside DllMain (loader-lock deadlock).
    auto* payload = new LoaderPayload{cfg, std::move(vi)};
    HANDLE th = ::CreateThread(nullptr, 0, &loader_thread_main, payload, 0, nullptr);
    if (!th) {
        LOG_ERROR("failed to spawn plugin loader thread; running without plugins");
        delete payload;
        return true;  // core stays loaded, forwards exports, no hooks
    }
    // core_shutdown owns the handle now (joins, then CloseHandle). do NOT CloseHandle here.
    g_loader_thread.store(th, std::memory_order_release);

    LOG_INFO("ck3accel_core init complete (loader thread spawned)");
    return true;
}

void core_shutdown() {
    LOG_INFO("ck3accel_core detaching");

    // join the loader before touching kill_switch / crash_sentinel state it may
    // still be building. 5 s cap so a wedged LoadLibrary can't hang teardown.
    // null handle on the Unknown-build path skips this.
    if (HANDLE h = g_loader_thread.exchange(nullptr, std::memory_order_acquire)) {
        ::WaitForSingleObject(h, 5000);
        ::CloseHandle(h);
    }

    kill_switch_stop();
    crash_sentinel_shutdown();   // joins the fallback timer thread
    telemetry_flush();
    hook_engine_shutdown();
    disable_console_logging();   // no-op if console was never enabled
}

} // namespace ck3accel
