// accel_eval_probe: DEVELOPER-ONLY trigger/dispatch/freeze probe (NOT SHIPPED).
//
// Measures whether Jomini trigger/event evaluation is a significant fraction of the late-game tick
// (the GO/NO-GO), and captures what owns the child-birth freeze. Observe-only MinHook hooks; never
// mutates game state. SP-only; off until eval_probe.conf enables it. See README.md.

#include <ck3accel/core_api.h>
#include <ck3accel/eval_accumulators.h>
#include <ck3accel/function_table.h>
#include <ck3accel/long_day_ring.h>
#include <ck3accel/probe_config.h>
#include <ck3accel/sample_histogram.h>
#include <ck3accel/tsc_clock.h>

#include <windows.h>
#include <timeapi.h>
#include <intrin.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

// x64-only: reads CONTEXT.Rip and the IMAGE_NT_HEADERS64 optional header. ck3.exe is x64.
static_assert(sizeof(void*) == 8, "accel_eval_probe is x64-only (CONTEXT.Rip)");

#if defined(_WIN32)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif

namespace {
constexpr int kLogInfo = 2;
constexpr int kLogWarn = 3;

const CoreApi* g_host = nullptr;
std::uintptr_t g_module_base = 0;
ck3accel::EvalProbeConfig g_cfg;

std::uint64_t g_tsc_per_sec = 0;
double        g_qpc_to_ms   = 0.0;
LONGLONG      g_qpc_freq    = 0;
FILE*         g_csv         = nullptr;
bool          g_csv_open_attempted = false;
const char* const kHookNames[ck3accel::HOOK_COUNT] = {
    "dispatch", "trigger", "eventtarget", "eventscope", "link"
};

// Resolved hook addresses (null = not found / not enabled).
void* g_addr_dispatch    = nullptr;  // 0x00817C20
void* g_addr_trigger     = nullptr;  // 0x0334C550
void* g_addr_eventtarget = nullptr;  // 0x0336AA90
void* g_addr_eventscope  = nullptr;  // 0x033598C6
void* g_addr_link        = nullptr;  // 0x0340F790
void* g_addr_tick        = nullptr;  // 0x0204EFE0 (freeze)

ck3accel::ShardRegistry g_shards;

// Per-thread accumulators (one shard per thread, registered once on first entry).
thread_local ck3accel::HookAccumulator t_acc[ck3accel::HOOK_COUNT] = {};
thread_local bool t_registered = false;

inline void ensure_registered() {
    if (!t_registered) { t_registered = true; g_shards.register_shard(t_acc); }
}

// Typed trampolines captured by install_hook.
using dispatch_fn = void  (*)(void* self);
using trigger_fn  = char  (*)(void* scope, void* node, char silent);
// eventtarget (0x0336AA90) is a C++ member fn returning a 16-byte struct via a
// hidden sret pointer in rdx (echoed in rax), with TWO trailing stack args. We
// model the sret buffer as an explicit rdx pointer arg and return void* (rax);
// a4=[rsp+0x20] bool, a5=[rsp+0x28] int32 loop-bound (load-bearing). Forward ALL six.
using et_fn       = void* (*)(void* chain, void* out_sret, void* scope, void* a3,
                              unsigned char a4, int a5);
// eventscope (0x033598C0, the TRUE entry) returns void; rdx is a 32-bit key, not a ptr.
using es_fn       = void  (*)(void* ctx, void* key32, void* keyblob, char flag);
using link_fn     = void* (*)(void* self, void* out, void* scopectx);
using tick_fn     = int   (*)(void* gamestate);

dispatch_fn g_orig_dispatch    = nullptr;
trigger_fn  g_orig_trigger     = nullptr;
et_fn       g_orig_eventtarget = nullptr;
es_fn       g_orig_eventscope  = nullptr;
link_fn     g_orig_link        = nullptr;
tick_fn     g_orig_tick        = nullptr;

// g_tick_tid is the thread executing UpdateTurnTick. CK3 runs the daily tick on a single sim
// thread, so the freeze burst samples exactly that thread; if a future build moved the tick onto
// the job pool this TID could be stale (sampling stays safe, suspend/resume of any live thread is
// harmless, but could mislead).
std::atomic<DWORD>          g_tick_tid{0};      // sim-thread id, published per day
std::atomic<bool>           g_in_tick{false};   // a day-tick is currently running
std::atomic<unsigned long long> g_tick_deadline_qpc{0};  // arm point for watchdog
std::atomic<std::uint64_t>  g_day_index{0};

// Snapshot of the global per-hook totals at the END of the previous day, so each
// day's record carries the DELTA. Touched only by the (single) sim thread.
ck3accel::HookStats g_prev_day_stats[ck3accel::HOOK_COUNT] = {};
ck3accel::LongDayRing<64> g_day_ring;            // sim-thread only

FILE* g_freeze_csv = nullptr;
bool  g_freeze_csv_open_attempted = false;
// Serializes all access to g_freeze_csv / g_freeze_csv_open_attempted across the
// sim thread (write_freeze_row) and the dump/watchdog thread (freeze_burst).
// ensure_freeze_csv_open() is ALWAYS called by a caller already holding this lock
// and must NOT take it itself (that would self-deadlock).
std::mutex g_freeze_mutex;

// Defined below; forward-declared so freeze_burst() (which precedes the
// definition) can open the CSV under g_freeze_mutex before its first write.
void ensure_freeze_csv_open();

ck3accel::FunctionTable g_functions;     // ck3.exe .pdata; built in Init, read-only
std::uint32_t           g_image_size = 0;
constexpr std::size_t   kBurstCap   = 4096;   // max RIP samples per freeze
constexpr std::uint32_t kRvaNonCk3  = 0xFFFFFFFFu;

// Signatures (facts about a public binary; no ck3.exe bytes shipped).
const char* const kSigDispatch =
    "48 89 5C 24 18 55 56 41 56 48 83 EC 20 48 8B 71 08 44 8B 76 1C "
    "41 8B DE 8B 6E 14 F0 0F C1 1E 3B DD 7D 4B";
const char* const kSigTrigger =
    "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 "
    "48 8D A8 C8 FD FF FF 48 81 EC 10 03 00 00";
const char* const kSigEventTarget =
    "48 89 5C 24 10 4C 89 4C 24 20 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8D AC 24 A0 FD FF FF";
const char* const kSigEventScope =
    // TRUE entry 0x033598C0 (push rsi; sub rsp,0x30), NOT 0x033598C6, which is 6 bytes into
    // the prologue (hooking mid-fn there gives 0 calls + a corruption landmine).
    "40 56 48 83 EC 30 48 89 6C 24 48 41 0F B6 E9 48 89 7C 24 28 49 8B F8 E8 ?? ?? ?? ?? 48 8B F0";
const char* const kSigLink =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 49 8B 40 28 49 8B F0 48 8B FA";
const char* const kSigTick =
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FD FF FF "
    "48 81 EC 88 03 00 00 0F 29 B4 24";

// Resolve the install dir (parent of \plugins): GetModuleFileNameW on our own HMODULE, then
// strip the trailing \plugins\<dll>.
std::wstring self_dll_directory() {
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&self_dll_directory),
            &self)) {
        return std::wstring();
    }
    std::wstring buf;
    buf.resize(MAX_PATH);
    for (;;) {
        DWORD len = ::GetModuleFileNameW(self, buf.data(),
                                         static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return std::wstring();
        }
        if (len < buf.size()) {
            buf.resize(len);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    // Strip \plugins\<dll> (two components) to reach the install dir.
    const std::wstring::size_type last = buf.find_last_of(L"\\/");
    if (last == std::wstring::npos) {
        return std::wstring();
    }
    const std::wstring plugins_dir = buf.substr(0, last);          // ...\plugins
    const std::wstring::size_type prev = plugins_dir.find_last_of(L"\\/");
    if (prev == std::wstring::npos) {
        return plugins_dir;  // unexpected layout; fall back to the dll's folder
    }
    return plugins_dir.substr(0, prev);                            // install dir
}

std::string read_config_text() {
    const std::wstring install = self_dll_directory();
    if (install.empty()) return {};
    const std::wstring path = install + L"\\eval_probe.conf";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || f == nullptr) return {};
    std::string out;
    char buf[4096];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

void ensure_probe_csv_open() {
    if (g_csv || g_csv_open_attempted) return;
    g_csv_open_attempted = true;
    const std::wstring install = self_dll_directory();
    if (install.empty()) {
        if (g_host && g_host->log)
            g_host->log(kLogWarn, "accel_eval_probe: no install dir; CSV disabled");
        return;
    }
    const std::wstring logs = install + L"\\logs";
    ::CreateDirectoryW(logs.c_str(), nullptr);
    const std::wstring path = logs + L"\\eval_probe.csv";
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    const bool existed = (attrs != INVALID_FILE_ATTRIBUTES) &&
                         !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    if (_wfopen_s(&g_csv, path.c_str(), L"a") != 0 || g_csv == nullptr) {
        g_csv = nullptr;
        if (g_host && g_host->log)
            g_host->log(kLogWarn, "accel_eval_probe: cannot open eval_probe.csv");
        return;
    }
    if (!existed)
        std::fprintf(g_csv, "dump_seq,hook,calls,calls_per_sec,inclusive_ns,"
                            "pct_of_measured_tick_wall\n");
    std::fflush(g_csv);
}

// `wall_ns` is the elapsed wall time since the previous dump; used for calls/sec and as the
// denominator for pct_of_measured_tick_wall (TRIGGER/dispatch inclusive ns over wall, an
// upper-bound "share of wall the probe attributes").
void dump_probe(std::uint64_t dump_seq, double wall_ns) {
    ck3accel::HookStats stats[ck3accel::HOOK_COUNT];
    g_shards.snapshot(stats, ck3accel::HOOK_COUNT);
    ensure_probe_csv_open();
    for (std::size_t i = 0; i < ck3accel::HOOK_COUNT; ++i) {
        const std::uint64_t incl_ns =
            ck3accel::tsc_to_ns(stats[i].incl_tsc, g_tsc_per_sec);
        const double per_sec = (wall_ns > 0.0)
            ? static_cast<double>(stats[i].count) / (wall_ns / 1.0e9) : 0.0;
        const double pct = (wall_ns > 0.0)
            ? static_cast<double>(incl_ns) / wall_ns * 100.0 : 0.0;
        if (g_host && g_host->log) {
            char line[192];
            std::snprintf(line, sizeof(line),
                "eval %-11s calls=%llu (%.0f/s) incl_ns=%llu pct_wall=%.2f%%",
                kHookNames[i],
                static_cast<unsigned long long>(stats[i].count), per_sec,
                static_cast<unsigned long long>(incl_ns), pct);
            g_host->log(kLogInfo, line);
        }
        if (g_csv)
            std::fprintf(g_csv, "%llu,%s,%llu,%.1f,%llu,%.4f\n",
                static_cast<unsigned long long>(dump_seq), kHookNames[i],
                static_cast<unsigned long long>(stats[i].count), per_sec,
                static_cast<unsigned long long>(incl_ns), pct);
    }
    if (g_csv) std::fflush(g_csv);
    if (g_host && g_host->report_metric) {
        const std::uint64_t trig_ns =
            ck3accel::tsc_to_ns(stats[ck3accel::HOOK_TRIGGER].incl_tsc, g_tsc_per_sec);
        const double trig_pct = (wall_ns > 0.0)
            ? static_cast<double>(trig_ns) / wall_ns * 100.0 : 0.0;
        g_host->report_metric("accel_eval_probe.trigger_pct_wall", trig_pct);
    }
}

// Parse ck3.exe's .pdata (the x64 exception RUNTIME_FUNCTION table) into g_functions and capture
// g_image_size. Reads the PE headers off the loaded image, like core/src/pe_inspect.cpp. Returns
// the number of function ranges added, or 0 on any header-validation failure. (Copied from
// plugins/_dev_sampler/src/plugin.cpp; only the log prefix differs.)
std::size_t build_function_table_from_pdata() {
    const auto* base = reinterpret_cast<const std::uint8_t*>(g_module_base);
    if (base == nullptr) {
        return 0;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }

    const auto* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }
    // Reject anything that is not a 64-bit image before reading the 64-bit
    // OptionalHeader fields (SizeOfImage / DataDirectory). ck3.exe is always x64.
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        return 0;
    }

    g_image_size = nt->OptionalHeader.SizeOfImage;

    const IMAGE_DATA_DIRECTORY& exception_dir =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (exception_dir.VirtualAddress == 0 || exception_dir.Size == 0) {
        return 0;  // no .pdata (unexpected for ck3.exe, but bail safely)
    }

    // Defense-in-depth: reject a malformed exception directory whose extent
    // exceeds the image. Use 64-bit arithmetic to avoid 32-bit overflow of the
    // sum (VirtualAddress and Size are both DWORD, so their sum can wrap).
    {
        const std::uint64_t pdata_end =
            static_cast<std::uint64_t>(exception_dir.VirtualAddress) +
            static_cast<std::uint64_t>(exception_dir.Size);
        if (pdata_end > static_cast<std::uint64_t>(g_image_size)) {
            if (g_host && g_host->log) {
                g_host->log(kLogWarn,
                    "accel_eval_probe: .pdata extent exceeds SizeOfImage; "
                    "malformed PE header — profiler inert");
            }
            return 0;
        }
    }

    // _IMAGE_RUNTIME_FUNCTION_ENTRY { DWORD BeginAddress, EndAddress, UnwindData }.
    const auto* fns = reinterpret_cast<const _IMAGE_RUNTIME_FUNCTION_ENTRY*>(
        base + exception_dir.VirtualAddress);
    const std::size_t count = exception_dir.Size / sizeof(_IMAGE_RUNTIME_FUNCTION_ENTRY);

    for (std::size_t i = 0; i < count; ++i) {
        const _IMAGE_RUNTIME_FUNCTION_ENTRY& e = fns[i];
        // add() drops degenerate/empty ranges (end <= begin) for us.
        g_functions.add(e.BeginAddress, e.EndAddress);
    }
    g_functions.finalize();
    return g_functions.size();
}

// RIP-burst-sample ONLY the sim thread while the current day is overrunning.
// Mirrors the accel_sampler critical-window invariant: between SuspendThread and
// ResumeThread do NOTHING but read Rip into the preallocated buffer; resume
// immediately; AT MOST ONE thread suspended at a time; re-check the kill switch
// before each suspend. Bucketing + CSV happen AFTER the burst.
void freeze_burst() {
    const DWORD tid = g_tick_tid.load(std::memory_order_acquire);
    if (tid == 0) return;
    HANDLE th = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
    if (th == nullptr) return;

    static std::uintptr_t rips[kBurstCap];   // daemon-thread-only; safe as static
    std::size_t n = 0;
    while (n < kBurstCap && g_in_tick.load(std::memory_order_acquire)) {
        if (g_host && g_host->is_kill_switch_active &&
            g_host->is_kill_switch_active() != 0)
            break;  // do not suspend if the core is about to MinHook-freeze threads
        if (::SuspendThread(th) == static_cast<DWORD>(-1)) break;
        CONTEXT ctx; ctx.ContextFlags = CONTEXT_CONTROL;
        const BOOL ok = ::GetThreadContext(th, &ctx);
        const std::uintptr_t rip = ok ? static_cast<std::uintptr_t>(ctx.Rip) : 0;
        ::ResumeThread(th);                  // resume IMMEDIATELY, always
        if (ok && rip != 0) rips[n++] = rip;
        ::Sleep(0);  // brief yield so we don't pin a core; coarse is fine
    }
    ::CloseHandle(th);

    ck3accel::SampleHistogram hist;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uintptr_t rip = rips[i];
        if (g_image_size != 0 && rip >= g_module_base &&
            rip < g_module_base + g_image_size)
            hist.add(g_functions.lookup(
                static_cast<std::uint32_t>(rip - g_module_base)));
        else
            hist.add(kRvaNonCk3);
    }
    if (g_host && g_host->log) {
        char msg[128];
        std::snprintf(msg, sizeof(msg),
            "accel_eval_probe: freeze burst captured %zu samples; top functions:", n);
        g_host->log(kLogWarn, msg);
        for (const auto& e : hist.top_n(15)) {
            char line[96];
            std::snprintf(line, sizeof(line), "   rva=0x%08X samples=%llu",
                e.rva, static_cast<unsigned long long>(e.count));
            g_host->log(kLogInfo, line);
        }
    }
    {
        // Serialize with write_freeze_row (sim thread). Open the file FIRST so the first burst
        // (which fires DURING the overrunning day, before any long-day row has opened the file) is
        // not silently dropped. Tag each row with the current day index: detour_tick increments
        // g_day_index only AFTER the day ends, so during the day it equals the index this day's
        // data row will get, making burst attribution unambiguous regardless of interleaving.
        std::lock_guard<std::mutex> lk(g_freeze_mutex);
        ensure_freeze_csv_open();
        if (g_freeze_csv) {
            const unsigned long long day = g_day_index.load(std::memory_order_relaxed);
            for (const auto& e : hist.top_n(15))
                std::fprintf(g_freeze_csv, "# burst day=%llu rva=0x%08X samples=%llu\n",
                    day, e.rva, static_cast<unsigned long long>(e.count));
            std::fflush(g_freeze_csv);
        }
    }
}

void ensure_freeze_csv_open() {
    if (g_freeze_csv || g_freeze_csv_open_attempted) return;
    g_freeze_csv_open_attempted = true;
    const std::wstring install = self_dll_directory();
    if (install.empty()) return;
    const std::wstring logs = install + L"\\logs";
    ::CreateDirectoryW(logs.c_str(), nullptr);
    const std::wstring path = logs + L"\\eval_freeze.csv";
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    const bool existed = (attrs != INVALID_FILE_ATTRIBUTES) &&
                         !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    if (_wfopen_s(&g_freeze_csv, path.c_str(), L"a") != 0 || g_freeze_csv == nullptr) {
        g_freeze_csv = nullptr;
        return;
    }
    if (!existed)
        std::fprintf(g_freeze_csv,
            "day_index,wall_ms,d_dispatch,d_trigger,d_eventtarget,"
            "d_eventscope,d_link\n");
    std::fflush(g_freeze_csv);
}

void write_freeze_row(const ck3accel::LongDayRecord& r) {
    // Serialize with freeze_burst (dump thread). ensure_freeze_csv_open() runs
    // under this lock and must not take it again.
    std::lock_guard<std::mutex> lk(g_freeze_mutex);
    ensure_freeze_csv_open();
    if (!g_freeze_csv) return;
    std::fprintf(g_freeze_csv, "%llu,%.2f,%llu,%llu,%llu,%llu,%llu\n",
        static_cast<unsigned long long>(r.day_index), r.wall_ms,
        static_cast<unsigned long long>(r.hooks[ck3accel::HOOK_DISPATCH].count),
        static_cast<unsigned long long>(r.hooks[ck3accel::HOOK_TRIGGER].count),
        static_cast<unsigned long long>(r.hooks[ck3accel::HOOK_EVENTTARGET].count),
        static_cast<unsigned long long>(r.hooks[ck3accel::HOOK_EVENTSCOPE].count),
        static_cast<unsigned long long>(r.hooks[ck3accel::HOOK_LINK].count));
    std::fflush(g_freeze_csv);
}

DWORD WINAPI dump_thread_main(LPVOID /*param*/) {
    // Raise the system timer resolution to ~1 ms so the Sleep(2) watchdog poll and Sleep(0) burst
    // spacing run finely, not at the default ~15.6 ms (else the watchdog arms its burst up to ~15 ms
    // late and samples coarsely). Matches _dev_sampler. Daemon thread to process exit, so the OS
    // releases the period at exit; no timeEndPeriod needed.
    ::timeBeginPeriod(1);

    // Calibrate TSC against QPC over a ~200 ms sleep.
    LARGE_INTEGER q0, q1;
    const std::uint64_t r0 = __rdtsc();
    ::QueryPerformanceCounter(&q0);
    ::Sleep(200);
    const std::uint64_t r1 = __rdtsc();
    ::QueryPerformanceCounter(&q1);
    g_tsc_per_sec = ck3accel::calibrate_tsc_per_sec(
        r1 - r0,
        static_cast<std::uint64_t>(q1.QuadPart - q0.QuadPart),
        static_cast<std::uint64_t>(g_qpc_freq));

    LARGE_INTEGER last; ::QueryPerformanceCounter(&last);
    std::uint64_t dump_seq = 0;
    const double dump_ns = static_cast<double>(g_cfg.dump_interval_s) * 1.0e9;
    for (;;) {
        ::Sleep(2);  // fine poll so we can catch a freeze promptly
        LARGE_INTEGER now; ::QueryPerformanceCounter(&now);

        // Watchdog: a day is running AND has passed its deadline -> burst.
        if (g_cfg.freeze && g_in_tick.load(std::memory_order_acquire)) {
            const unsigned long long dl =
                g_tick_deadline_qpc.load(std::memory_order_acquire);
            if (dl != 0 && static_cast<unsigned long long>(now.QuadPart) >= dl) {
                freeze_burst();   // returns when the day ends or the cap is hit
            }
        }

        // Dump on cadence.
        const double wall_ns =
            static_cast<double>(now.QuadPart - last.QuadPart) * g_qpc_to_ms * 1.0e6;
        if (wall_ns >= dump_ns) {
            dump_probe(++dump_seq, wall_ns);
            last = now;
        }
    }
    // unreachable (daemon thread)
}

// Hot detour discipline: read __rdtsc enter/exit, depth-guarded accumulate into
// the TLS shard, call the trampoline, return its result UNCHANGED. No map, no
// lock, no heap alloc, no logging, no CoreApi calls in these bodies.
//
// No re-entry flag is used: the depth-guarded accumulator (eval_accumulators.h)
// already makes recursive trigger->sub-trigger->event-target nesting correct
// (inclusive time at the outermost frame only; every call counted). A wrapping
// re-entry flag would instead break the recursive call-count.

void detour_dispatch(void* self) {
    ensure_registered();
    auto& a = t_acc[ck3accel::HOOK_DISPATCH];
    a.on_enter(__rdtsc());
    g_orig_dispatch(self);
    a.on_exit(__rdtsc());
}

char detour_trigger(void* scope, void* node, char silent) {
    ensure_registered();
    auto& a = t_acc[ck3accel::HOOK_TRIGGER];
    a.on_enter(__rdtsc());
    const char rc = g_orig_trigger(scope, node, silent);
    a.on_exit(__rdtsc());
    return rc;
}

void* detour_eventtarget(void* chain, void* out_sret, void* scope, void* a3,
                         unsigned char a4, int a5) {
    ensure_registered();
    auto& a = t_acc[ck3accel::HOOK_EVENTTARGET];
    a.on_enter(__rdtsc());
    void* rc = g_orig_eventtarget(chain, out_sret, scope, a3, a4, a5);  // forward all 6
    a.on_exit(__rdtsc());
    return rc;  // the sret pointer echoed in rax
}

void detour_eventscope(void* ctx, void* key32, void* keyblob, char flag) {
    ensure_registered();
    auto& a = t_acc[ck3accel::HOOK_EVENTSCOPE];
    a.on_enter(__rdtsc());
    g_orig_eventscope(ctx, key32, keyblob, flag);  // void return
    a.on_exit(__rdtsc());
}

void* detour_link(void* self, void* out, void* scopectx) {
    ensure_registered();
    auto& a = t_acc[ck3accel::HOOK_LINK];
    a.on_enter(__rdtsc());
    void* rc = g_orig_link(self, out, scopectx);
    a.on_exit(__rdtsc());
    return rc;
}

// UpdateTurnTick (per-simulated-day boundary). Runs on the sim thread and suspends nobody, so
// snapshot()/CSV/log after the trampoline is safe (unlike the hot detours). Fires ~0.6-8x/sec;
// negligible overhead. Publishes sim-thread id + in-tick flag + a watchdog deadline (consumed
// there), times each day, and records per-hook DELTAS into a rolling ring + (for long days)
// logs/eval_freeze.csv.
int detour_tick(void* gamestate) {
    const DWORD tid = ::GetCurrentThreadId();
    g_tick_tid.store(tid, std::memory_order_release);

    LARGE_INTEGER t0; ::QueryPerformanceCounter(&t0);
    const unsigned long long deadline =
        static_cast<unsigned long long>(t0.QuadPart) +
        static_cast<unsigned long long>(
            static_cast<double>(g_cfg.freeze_threshold_ms) / g_qpc_to_ms);
    g_tick_deadline_qpc.store(deadline, std::memory_order_release);
    g_in_tick.store(true, std::memory_order_release);

    const int rc = g_orig_tick(gamestate);   // run the day (observe only)

    g_in_tick.store(false, std::memory_order_release);
    LARGE_INTEGER t1; ::QueryPerformanceCounter(&t1);
    const double wall_ms =
        static_cast<double>(t1.QuadPart - t0.QuadPart) * g_qpc_to_ms;

    // Build this day's record with per-hook DELTAS vs the previous day.
    ck3accel::LongDayRecord rec;
    rec.day_index = g_day_index.fetch_add(1, std::memory_order_relaxed);
    rec.wall_ms = wall_ms;
    ck3accel::HookStats cur[ck3accel::HOOK_COUNT];
    g_shards.snapshot(cur, ck3accel::HOOK_COUNT);
    for (std::size_t i = 0; i < ck3accel::HOOK_COUNT; ++i) {
        rec.hooks[i].count =
            (cur[i].count >= g_prev_day_stats[i].count)
                ? cur[i].count - g_prev_day_stats[i].count : 0;
        rec.hooks[i].incl_tsc =
            (cur[i].incl_tsc >= g_prev_day_stats[i].incl_tsc)
                ? cur[i].incl_tsc - g_prev_day_stats[i].incl_tsc : 0;
        g_prev_day_stats[i] = cur[i];
    }
    g_day_ring.push(rec);
    if (wall_ms >= static_cast<double>(g_cfg.freeze_threshold_ms)) {
        write_freeze_row(rec);   // a long day (e.g. child-birth freeze)
        if (g_host && g_host->log) {
            char msg[160];
            std::snprintf(msg, sizeof(msg),
                "accel_eval_probe: LONG DAY #%llu took %.1f ms "
                "(trigger calls=%llu)",
                static_cast<unsigned long long>(rec.day_index), wall_ms,
                static_cast<unsigned long long>(rec.hooks[ck3accel::HOOK_TRIGGER].count));
            g_host->log(kLogWarn, msg);
        }
    }
    return rc;
}

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)),  // struct_size (FIRST)
    CK3ACCEL_PLUGIN_MAGIC,                              // magic
    CK3ACCEL_ABI_VERSION,                              // required_abi
    "accel_eval_probe",                                // name (allowlist key)
    "0.1.0",                                           // semver
    "any",                                             // min_game_version
    "any",                                             // max_game_version
    CK3ACCEL_MODE_SP,                                  // SP-only (dev profiling tool)
};
}  // namespace

PLUGIN_EXPORT const CK3AccelPluginInfo* CK3Accel_Query(uint32_t host_abi_version) {
    (void)host_abi_version;
    return &kInfo;
}

PLUGIN_EXPORT int CK3Accel_Init(const CoreApi* host, CK3AccelRegistrar* reg) {
    g_host = host;
    if (!host || !host->log || !host->scan || !host->install_hook) {
        return 1;  // host too old: stay inert
    }
    if (!reg) return 1;

    g_module_base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    LARGE_INTEGER freq;
    if (::QueryPerformanceFrequency(&freq) && freq.QuadPart != 0) {
        g_qpc_freq  = freq.QuadPart;
        g_qpc_to_ms = 1.0e3 / static_cast<double>(freq.QuadPart);
    }
    g_cfg = ck3accel::parse_eval_probe_config(read_config_text());

    if (!g_cfg.enabled) {
        host->log(kLogInfo,
            "accel_eval_probe: disabled (set eval_probe=true in eval_probe.conf "
            "next to the dll); inert");
        return 0;  // loaded but inert by design
    }

    auto resolve = [&](bool want, const char* sig, void** out, const char* label) {
        if (!want) return;
        void* a = host->scan(sig);
        *out = a;
        char msg[160];
        if (a) {
            std::snprintf(msg, sizeof(msg),
                "accel_eval_probe: resolved %s @ 0x%llX (RVA 0x%llX)", label,
                static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(a)),
                static_cast<unsigned long long>(
                    reinterpret_cast<std::uintptr_t>(a) - g_module_base));
            host->log(kLogInfo, msg);
        } else {
            std::snprintf(msg, sizeof(msg),
                "accel_eval_probe: %s signature NOT FOUND; that hook inert", label);
            host->log(kLogWarn, msg);
        }
    };
    resolve(g_cfg.dispatch,    kSigDispatch,    &g_addr_dispatch,    "dispatch");
    resolve(g_cfg.trigger,     kSigTrigger,     &g_addr_trigger,     "trigger");
    resolve(g_cfg.eventtarget, kSigEventTarget, &g_addr_eventtarget, "eventtarget");
    resolve(g_cfg.eventscope,  kSigEventScope,  &g_addr_eventscope,  "eventscope");
    resolve(g_cfg.link,        kSigLink,        &g_addr_link,        "link");
    resolve(g_cfg.freeze,      kSigTick,        &g_addr_tick,        "freeze/UpdateTurnTick");

    if (g_cfg.freeze && g_addr_tick) {
        const std::size_t fn_count = build_function_table_from_pdata();
        if (fn_count == 0 || g_image_size == 0) {
            host->log(kLogWarn,
                "accel_eval_probe: could not parse ck3.exe .pdata; freeze burst "
                "disabled (per-day timing still active)");
            // Keep g_addr_tick so per-day timing/CSV still works; the watchdog
            // simply has no function table to bucket against and will skip
            // bucketing. (build_function_table_from_pdata left g_functions empty.)
        }
    }

    auto install = [&](void* target, void* detour, void** orig, const char* label) {
        if (!target) return;
        host->install_hook(reg->hook_set, target, detour, orig);
        char msg[128];
        std::snprintf(msg, sizeof(msg), "accel_eval_probe: hooked %s", label);
        host->log(kLogInfo, msg);
    };
    install(g_addr_dispatch,    reinterpret_cast<void*>(&detour_dispatch),
            reinterpret_cast<void**>(&g_orig_dispatch),    "dispatch");
    install(g_addr_trigger,     reinterpret_cast<void*>(&detour_trigger),
            reinterpret_cast<void**>(&g_orig_trigger),     "trigger");
    install(g_addr_eventtarget, reinterpret_cast<void*>(&detour_eventtarget),
            reinterpret_cast<void**>(&g_orig_eventtarget), "eventtarget");
    install(g_addr_eventscope,  reinterpret_cast<void*>(&detour_eventscope),
            reinterpret_cast<void**>(&g_orig_eventscope),  "eventscope");
    install(g_addr_link,        reinterpret_cast<void*>(&detour_link),
            reinterpret_cast<void**>(&g_orig_link),        "link");
    install(g_addr_tick,        reinterpret_cast<void*>(&detour_tick),
            reinterpret_cast<void**>(&g_orig_tick),        "freeze/UpdateTurnTick");

    host->log(kLogInfo, "accel_eval_probe: enabled hooks installed and active");

    if (HANDLE th = ::CreateThread(nullptr, 0, &dump_thread_main, nullptr, 0, nullptr))
        ::CloseHandle(th);
    else
        host->log(kLogWarn, "accel_eval_probe: dump thread failed to start");
    return 0;
}
