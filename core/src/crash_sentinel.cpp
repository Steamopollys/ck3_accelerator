#include "crash_sentinel.h"

#include "hook_engine.h"   // ck3accel::try_address_in_hooked_range, try_disable_all
#include "logger.h"
#include "telemetry.h"     // ck3accel::telemetry_flush_try

#include <windows.h>
#include <psapi.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace ck3accel {

// ============================================================================
// crash-path non-blocking design
// ----------------------------------------------------------------------------
// VEH / SetUnhandledExceptionFilter run on the FAULTING thread in arbitrary
// context. it may already hold the hook-engine mutex (faulted inside install_hook,
// MinHook under the lock, or disable_all) or the telemetry mutex (mid
// report_metric); re-acquiring those non-recursive mutexes from the handler would
// self-deadlock, turning a recoverable crash into a hang. so the live handlers are
// best-effort and NEVER block on a mutex.
//
// durable safe-mode is the on-disk tombstone, written at install before any hook
// can fault and cleared only at stability; it guarantees next-launch per-plugin
// safe-mode no matter what the handlers manage live. the handlers only try to
// improve the crash (name the arming plugin, soft-disable hooks, flush telemetry)
// and bail on contention:
//   * reads of the tombstone path / content / module ranges use lock-free atomics.
//   * attribution: try_address_in_hooked_range (try_lock). on contention it is
//     undetermined: don't claim unverified attribution, but still check the
//     lock-free registered-module ranges.
//   * hook disable: try_disable_all (try_lock).
//   * telemetry flush: telemetry_flush_try (try_lock).
//   * the handler's tombstone write uses raw Win32 CreateFileW/WriteFile on a
//     pre-formatted buffer (no ofstream / heap churn under fault).
// any failed try_lock: do the minimal safe thing and return
// EXCEPTION_CONTINUE_SEARCH; the install-time tombstone still trips next-launch.
// ============================================================================

// ------------------------------------------------------------------ pure layer

void write_tombstone(const std::filesystem::path& p, const Tombstone& t) {
    std::ofstream os(p, std::ios::binary | std::ios::trunc);
    if (!os) {
        return;
    }
    os << "game_version="  << t.game_version  << '\n';
    os << "timestamp_utc=" << t.timestamp_utc << '\n';
    os << "arming_plugin=" << t.arming_plugin << '\n';
}

std::optional<Tombstone> read_tombstone(const std::filesystem::path& p) {
    std::ifstream is(p, std::ios::binary);
    if (!is) {
        return std::nullopt;
    }

    Tombstone t;
    bool have_version   = false;
    bool have_timestamp = false;

    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) {
            return std::nullopt;  // malformed: a non-blank line with no '='
        }
        const std::string key   = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "game_version") {
            t.game_version = value;
            have_version = true;
        } else if (key == "timestamp_utc") {
            t.timestamp_utc = value;
            have_timestamp = true;
        } else if (key == "arming_plugin") {
            t.arming_plugin = value;
        } else {
            return std::nullopt;  // malformed: unrecognized key
        }
    }

    if (!have_version || !have_timestamp) {
        return std::nullopt;
    }
    return t;
}

void clear_tombstone(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::remove(p, ec);
}

// --------------------------------------------------------------- runtime layer

namespace {

// max plugin module ranges tracked for attribution. bounded so the crash-path
// scan is a fixed-size, lock-free array read.
constexpr std::size_t kMaxModules = 64;

struct SentinelState {
    // --- configuration / non-crash-path state, guarded by `mutex` ---
    std::mutex            mutex;
    Tombstone             tombstone;        // current logical contents
    bool                  installed = false;

    // --- crash-path-visible state (lock-free) ---
    // wide tombstone path, set ONCE at install before path_ready. handler reads it
    // only when path_ready (acquire).
    std::wstring          tombstone_path_w;
    std::atomic<bool>     path_ready{false};

    // pre-formatted tombstone bytes, published by atomic pointer swap. handler
    // reads the pointer (acquire) and writes those exact bytes. superseded buffers
    // are intentionally leaked (tiny, process-lifetime): freeing would race a
    // concurrent handler read.
    std::atomic<const std::string*> crash_blob{nullptr};

    // lock-free registered-module ranges. append-only, single-threaded at loader
    // startup; handler reads up to module_count (acquire).
    std::array<std::atomic<std::uintptr_t>, kMaxModules> mod_begin{};
    std::array<std::atomic<std::uintptr_t>, kMaxModules> mod_end{};
    std::atomic<std::size_t> module_count{0};

    // --- stability / shutdown ---
    std::once_flag        stable_once;

    std::thread             timer;
    std::condition_variable timer_cv;
    std::mutex              timer_mutex;
    bool                    timer_stop = false;

    PVOID                        veh_handle  = nullptr;
    LPTOP_LEVEL_EXCEPTION_FILTER prev_filter = nullptr;
};

SentinelState& state() {
    static SentinelState s;
    return s;
}

std::string now_utc_iso8601() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &t);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return os.str();
}

// serialize a tombstone to the on-disk key=value bytes.
std::string format_tombstone(const Tombstone& t) {
    std::string s;
    s.reserve(64 + t.game_version.size() + t.timestamp_utc.size() +
              t.arming_plugin.size());
    s += "game_version=";  s += t.game_version;  s += '\n';
    s += "timestamp_utc="; s += t.timestamp_utc; s += '\n';
    s += "arming_plugin="; s += t.arming_plugin; s += '\n';
    return s;
}

// publish a fresh crash blob for the current tombstone. caller holds s.mutex. the
// buffer is heap-allocated and leaked into the atomic (see crash_blob); allocation
// happens here on a normal thread, never on the crash path.
void publish_crash_blob(SentinelState& s) {
    auto* blob = new std::string(format_tombstone(s.tombstone));
    s.crash_blob.store(blob, std::memory_order_release);
}

// Crash-path tombstone write: raw Win32, no heap, no std::ofstream. Best-effort.
void crash_write_tombstone(SentinelState& s) {
    if (!s.path_ready.load(std::memory_order_acquire)) {
        return;
    }
    const std::string* blob = s.crash_blob.load(std::memory_order_acquire);
    if (blob == nullptr) {
        return;
    }
    HANDLE h = CreateFileW(s.tombstone_path_w.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(h, blob->data(), static_cast<DWORD>(blob->size()), &written,
              nullptr);
    CloseHandle(h);
}

// Lock-free scan of registered plugin module ranges.
bool rip_in_registered_module(SentinelState& s, std::uintptr_t rip) {
    const std::size_t n = s.module_count.load(std::memory_order_acquire);
    for (std::size_t i = 0; i < n && i < kMaxModules; ++i) {
        const std::uintptr_t b = s.mod_begin[i].load(std::memory_order_relaxed);
        const std::uintptr_t e = s.mod_end[i].load(std::memory_order_relaxed);
        if (rip >= b && rip < e) {
            return true;
        }
    }
    return false;
}

// VEH: act ONLY on access violations whose RIP is in a trampoline/detour we own
// or a registered plugin module. everything else passes through. NEVER blocks on
// a mutex (see top-of-file).
LONG CALLBACK veh_handler(EXCEPTION_POINTERS* info) {
    if (info == nullptr || info->ExceptionRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const void* rip =
        (info->ContextRecord != nullptr)
            ? reinterpret_cast<const void*>(info->ContextRecord->Rip)
            : nullptr;
    if (rip == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    SentinelState& s = state();

    // attribution: hook-engine range (non-blocking) OR the lock-free module
    // ranges. if the hook-engine lock is contended we can't verify the trampoline
    // span, so fall back to module ranges only; never fabricate attribution.
    bool determined = false;
    // determined is intentionally ignored: a contended/undetermined result is
    // treated as not-in-range; the on-disk tombstone still guarantees next-launch
    // safe-mode for the arming plugin.
    const bool in_hook_range =
        try_address_in_hooked_range(rip, &determined);
    const bool in_module =
        rip_in_registered_module(s, reinterpret_cast<std::uintptr_t>(rip));
    if (!(in_hook_range || in_module)) {
        return EXCEPTION_CONTINUE_SEARCH;  // not ours; pass through untouched
    }

    // in-range fault, ours. rewrite the tombstone (raw Win32; the blob already
    // names the arming plugin), soft-disable hooks, flush telemetry: all
    // best-effort, non-blocking.
    crash_write_tombstone(s);

    try_disable_all();
    telemetry_flush_try();

    // non-blocking, non-allocating breadcrumb: fixed wide literal only, no spdlog
    // (mutex + flush), no heap on the fault path.
    OutputDebugStringW(L"[ck3accel] crash sentinel: in-range access violation; "
                       L"tombstone written, plugin will be disabled next launch.\n");

    // let the OS continue its crash handling (we didn't fix the fault).
    return EXCEPTION_CONTINUE_SEARCH;
}

// Last-resort breadcrumb on a truly unhandled crash. Also non-blocking.
LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* info) {
    // non-blocking, non-allocating breadcrumb: fixed wide literal only, no spdlog
    // (mutex + flush), no heap on the fault path.
    OutputDebugStringW(L"[ck3accel] crash sentinel: unhandled exception reached "
                       L"top-level filter; tombstone written, plugin will be disabled next launch.\n");
    telemetry_flush_try();

    SentinelState& s = state();
    // prev_filter is published during single-threaded install before the handler
    // is armed; no fence needed to read it from the fault path.
    LPTOP_LEVEL_EXCEPTION_FILTER prev = s.prev_filter;
    if (prev != nullptr) {
        return prev(info);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void timer_body(int fallback_seconds) {
    SentinelState& s = state();
    std::unique_lock<std::mutex> lock(s.timer_mutex);
    s.timer_cv.wait_for(lock, std::chrono::seconds(fallback_seconds),
                        [&s] { return s.timer_stop; });
    const bool stopped = s.timer_stop;
    lock.unlock();
    if (!stopped) {
        // no stability anchor (e.g. no save loaded) in the window: assume stable
        // so the tombstone doesn't falsely trip safe-mode.
        crash_sentinel_mark_stable();
    }
}

} // namespace

void crash_sentinel_install(const std::filesystem::path& tombstone_path,
                            Tombstone initial,
                            int fallback_seconds) {
    SentinelState& s = state();

    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.installed) {
            return;  // idempotent
        }
        s.installed = true;
        s.tombstone = std::move(initial);
        if (s.tombstone.timestamp_utc.empty()) {
            s.tombstone.timestamp_utc = now_utc_iso8601();
        }

        // Write the durable tombstone NOW, before any hook can fault.
        write_tombstone(tombstone_path, s.tombstone);

        // publish crash-path state. the wide path is fully populated before
        // path_ready flips true, so the handler never reads a half-built path.
        s.tombstone_path_w = tombstone_path.wstring();
        publish_crash_blob(s);
        s.path_ready.store(true, std::memory_order_release);
    }

    // VEH runs before SEH; nonzero First keeps us ahead of the game's handler.
    s.veh_handle = AddVectoredExceptionHandler(/*First=*/1u, &veh_handler);
    if (s.veh_handle == nullptr) {
        LOG_WARN("Crash sentinel: AddVectoredExceptionHandler failed");
    }

    s.prev_filter = SetUnhandledExceptionFilter(&unhandled_filter);

    {
        std::ostringstream os;
        os << "Crash sentinel armed (tombstone=" << tombstone_path.string()
           << ", fallback=" << fallback_seconds << "s)";
        LOG_INFO(os.str());
    }

    if (fallback_seconds > 0) {
        s.timer = std::thread(&timer_body, fallback_seconds);
    }
}

void crash_sentinel_set_arming_plugin(std::string name) {
    SentinelState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.tombstone.arming_plugin = std::move(name);
    if (s.installed) {
        // refresh the file and the crash blob so a later fault names the right plugin.
        write_tombstone(std::filesystem::path(s.tombstone_path_w), s.tombstone);
        publish_crash_blob(s);
    }
}

void crash_sentinel_register_module(std::string name, const void* module_base) {
    SentinelState& s = state();
    if (module_base == nullptr) {
        return;
    }
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(),
                              reinterpret_cast<HMODULE>(const_cast<void*>(module_base)),
                              &mi, sizeof(mi))) {
        LOG_WARN("Crash sentinel: GetModuleInformation failed for plugin " + name);
        return;
    }
    const auto begin = reinterpret_cast<std::uintptr_t>(mi.lpBaseOfDll);
    const auto end   = begin + mi.SizeOfImage;

    // append to the lock-free range table. publish begin/end BEFORE bumping count
    // (release) so the handler never reads a slot past valid data.
    std::lock_guard<std::mutex> lock(s.mutex);
    const std::size_t idx = s.module_count.load(std::memory_order_relaxed);
    if (idx >= kMaxModules) {
        LOG_WARN("Crash sentinel: module range table full; not tracking " + name);
        return;
    }
    s.mod_begin[idx].store(begin, std::memory_order_relaxed);
    s.mod_end[idx].store(end, std::memory_order_relaxed);
    s.module_count.store(idx + 1, std::memory_order_release);
}

void crash_sentinel_mark_stable() {
    SentinelState& s = state();
    std::call_once(s.stable_once, [&s] {
        std::filesystem::path path;
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            path = std::filesystem::path(s.tombstone_path_w);
        }
        if (!path.empty()) {
            clear_tombstone(path);
        }

        // stop the fallback timer so it doesn't wake and re-clear.
        {
            std::lock_guard<std::mutex> lock(s.timer_mutex);
            s.timer_stop = true;
        }
        s.timer_cv.notify_all();

        LOG_INFO("Crash sentinel: marked stable; tombstone cleared");
    });
}

void crash_sentinel_shutdown() {
    SentinelState& s = state();
    {
        std::lock_guard<std::mutex> lock(s.timer_mutex);
        s.timer_stop = true;
    }
    s.timer_cv.notify_all();
    if (s.timer.joinable()) {
        s.timer.join();   // never destroy a joinable std::thread
    }
    if (s.veh_handle != nullptr) {
        RemoveVectoredExceptionHandler(s.veh_handle);
        s.veh_handle = nullptr;
    }
    SetUnhandledExceptionFilter(s.prev_filter);
}

} // namespace ck3accel
