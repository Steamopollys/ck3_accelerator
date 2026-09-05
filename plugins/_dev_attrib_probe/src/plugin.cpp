// accel_attrib_probe: DEVELOPER-ONLY attribution probe (NOT SHIPPED).
//
// Answers two questions the RIP-only samplers could not:
//   1. WHICH script trigger nodes account for the tens of millions of trigger evals on a
//      child-birth day (per-node count + inclusive cycles, named by RTTI class + trigger id), and
//      the true (node, entity) repeat rate / result consistency (the cache question, keyed this
//      time on the evaluated entity too).
//   2. WHAT the main thread does during a UI freeze (character window / court positions):
//      a message-pump stall watchdog captures CALL STACKS of the main thread, and the same stack
//      sampler runs on the sim thread for over-long simulated days (attributes memcpy & co. to
//      their callers).
//
// Observe-only: every hooked function's result is returned unchanged. SP-only; inert unless
// attrib_probe.conf enables it. Suspend/resume discipline (the accel_sampler invariant): between
// SuspendThread and ResumeThread nothing but register/stack reads into preallocated buffers; at
// most one thread suspended at a time; unwinding uses ONLY module exception tables we mapped at
// init (never RtlLookupFunctionEntry, which can take a loader lock the suspended thread might hold).

#include <ck3accel/attrib_tables.h>
#include <ck3accel/core_api.h>
#include <ck3accel/function_table.h>
#include <ck3accel/repeat_table.h>
#include <ck3accel/sample_histogram.h>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <timeapi.h>
#include <intrin.h>
#include <share.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif

static_assert(sizeof(void*) == 8, "accel_attrib_probe is x64-only");

namespace {
constexpr int kLogInfo = 2;
constexpr int kLogWarn = 3;

const CoreApi* g_host = nullptr;
std::uintptr_t g_module_base = 0;
inline bool game_ready();
std::uint32_t  g_image_size = 0;

struct Config {
    bool enabled = false;
    bool nodes = true;
    bool repeat = true;
    bool freeze_stacks = true;
    bool ui_stacks = true;
    int  freeze_threshold_ms = 150;
    int  ui_threshold_ms = 150;
    int  top_nodes = 100;
    long long dump_min_evals = 2000000;
} g_cfg;

// ---- signatures (facts about a public binary; no ck3.exe bytes shipped) ---------------------
const char* const kSigTrigger =   // jomini_trigger.cpp evaluator: char(node, scope, silent)
    "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 "
    "48 8D A8 C8 FD FF FF 48 81 EC 10 03 00 00";
const char* const kSigTick =      // UpdateTurnTick: int(gamestate)
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FD FF FF "
    "48 81 EC 88 03 00 00 0F 29 B4 24";

// ---- trigger-node attribution ---------------------------------------------------------------
constexpr std::size_t kNodeCap   = 1u << 17;   // 128K slots/thread (4 MiB)
constexpr std::size_t kRepeatCap = 1u << 19;   // 512K slots/thread (8 MiB)
constexpr std::size_t kRepeatProbe = 64;       // bounded probing: a full table overflows, never scans
struct Shard {
    ck3accel::NodeTable<kNodeCap> nodes;
    ck3accel::RepeatTable<kRepeatCap, kRepeatProbe>* repeat = nullptr;   // allocated only if cfg.repeat
};
thread_local Shard* t_shard = nullptr;
std::mutex g_shards_mtx;
std::vector<Shard*> g_shards;

Shard* ensure_shard() {
    if (!t_shard) {
        t_shard = new Shard();
        if (g_cfg.repeat) t_shard->repeat = new ck3accel::RepeatTable<kRepeatCap, kRepeatProbe>();
        std::lock_guard<std::mutex> lk(g_shards_mtx);
        g_shards.push_back(t_shard);
    }
    return t_shard;
}

using trigger_fn = char (*)(void* node, void* scope, char silent);
trigger_fn g_orig_trigger = nullptr;

inline std::uint64_t mix64(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// rcx = trigger node (static script object: [node+8] = trigger id), rdx = scope context whose
// first qword points at the current entity's 16-byte scope ref {u16 type, ..., u64 handle @+8}.
char detour_trigger(void* node, void* scope, char silent) {
    if (!game_ready()) return g_orig_trigger(node, scope, silent);   // loading: pass through
    Shard* sh = ensure_shard();
    const std::uint64_t t0 = __rdtsc();
    const char rc = g_orig_trigger(node, scope, silent);
    const std::uint64_t dt = __rdtsc() - t0;
    sh->nodes.record(reinterpret_cast<std::uint64_t>(node), dt);
    if (sh->repeat && scope) {
        const std::uint8_t* ref = *reinterpret_cast<const std::uint8_t* const*>(scope);
        if (ref) {
            std::uint16_t type; std::uint64_t handle;
            std::memcpy(&type, ref, 2);
            std::memcpy(&handle, ref + 8, 8);
            const std::uint64_t key = mix64(reinterpret_cast<std::uint64_t>(node)) ^
                                      mix64(handle) ^ (static_cast<std::uint64_t>(type) << 56);
            sh->repeat->record(key, static_cast<std::uint8_t>(rc));
        }
    }
    return rc;   // observe-only: unchanged
}

// ---- RTTI naming of a node (all reads validated against the image range or SEH-guarded) ------
bool in_image(const void* p, std::size_t n = 8) {
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
    return g_module_base != 0 && a >= g_module_base && a + n <= g_module_base + g_image_size;
}

// Returns false if anything looks wrong. name buffer receives the demangled-ish class name.
bool node_class_name(const void* node, char* name, std::size_t cap, std::uint32_t* trigger_id) {
    __try {
        const std::uint8_t* n = static_cast<const std::uint8_t*>(node);
        const std::uint8_t* vt = *reinterpret_cast<const std::uint8_t* const*>(n);
        if (!in_image(vt) || !in_image(vt - 8)) return false;
        const std::uint8_t* col = *reinterpret_cast<const std::uint8_t* const*>(vt - 8);
        if (!in_image(col, 24)) return false;
        std::uint32_t td_rva; std::memcpy(&td_rva, col + 0xC, 4);
        const std::uint8_t* td = reinterpret_cast<const std::uint8_t*>(g_module_base) + td_rva;
        if (!in_image(td, 0x20)) return false;
        const char* mangled = reinterpret_cast<const char*>(td + 0x10);
        if (mangled[0] != '.' || mangled[1] != '?') return false;
        // strip ".?AV" / ".?AU", drop "@@" terminators, keep it readable
        std::size_t o = 0;
        for (const char* p = mangled + 4; *p && o + 1 < cap && p < mangled + 400; ++p) {
            if (p[0] == '@' && p[1] == '@') { ++p; continue; }
            name[o++] = (*p == '@') ? ':' : *p;
        }
        name[o] = 0;
        std::memcpy(trigger_id, n + 8, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- module exception tables for lock-free unwinding ---------------------------------------
struct ModuleTable {
    std::uintptr_t base = 0;
    std::uint32_t size = 0;
    const RUNTIME_FUNCTION* pdata = nullptr;
    std::uint32_t count = 0;
    char name[64] = {};
};
std::vector<ModuleTable> g_modules;   // built at init, refreshed once at +60 s and on demand; watchdog-only reader
std::atomic<bool> g_modules_stale{false};   // set by the unwinder when a frame hits an unmapped module
std::mutex g_modules_mtx;
const std::uint8_t* g_raw_pdata = nullptr;   // ck3.exe .pdata (for FunctionTable + unwinding)
std::uint32_t g_raw_pdata_count = 0;
ck3accel::FunctionTable g_functions;

void map_modules() {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!::EnumProcessModules(::GetCurrentProcess(), mods, sizeof(mods), &needed)) return;
    const DWORD n = needed / sizeof(HMODULE);
    std::vector<ModuleTable> out;
    for (DWORD i = 0; i < n && i < 512; ++i) {
        MODULEINFO mi{};
        if (!::GetModuleInformation(::GetCurrentProcess(), mods[i], &mi, sizeof(mi))) continue;
        const auto* base = static_cast<const std::uint8_t*>(mi.lpBaseOfDll);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) continue;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) continue;
        const IMAGE_DATA_DIRECTORY& ex = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (ex.VirtualAddress == 0 || ex.Size == 0) continue;
        ModuleTable t;
        t.base = reinterpret_cast<std::uintptr_t>(base);
        t.size = mi.SizeOfImage;
        t.pdata = reinterpret_cast<const RUNTIME_FUNCTION*>(base + ex.VirtualAddress);
        t.count = ex.Size / sizeof(RUNTIME_FUNCTION);
        char nm[MAX_PATH];
        if (::GetModuleBaseNameA(::GetCurrentProcess(), mods[i], nm, sizeof(nm))) {
            std::strncpy(t.name, nm, sizeof(t.name) - 1);
        }
        out.push_back(t);
        if (t.base == g_module_base) {
            g_raw_pdata = reinterpret_cast<const std::uint8_t*>(t.pdata);
            g_raw_pdata_count = t.count;
        }
    }
    std::lock_guard<std::mutex> lk(g_modules_mtx);
    g_modules.swap(out);
}

const ModuleTable* module_for(std::uintptr_t rip) {
    for (const ModuleTable& m : g_modules)
        if (rip >= m.base && rip < m.base + m.size) return &m;
    return nullptr;
}

// Binary search a module's RUNTIME_FUNCTION table for rip (no locks; table is static memory).
const RUNTIME_FUNCTION* find_rf(const ModuleTable* m, std::uintptr_t rip) {
    const std::uint32_t rva = static_cast<std::uint32_t>(rip - m->base);
    std::uint32_t lo = 0, hi = m->count;
    while (lo < hi) {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        const RUNTIME_FUNCTION& rf = m->pdata[mid];
        if (rva < rf.BeginAddress) hi = mid;
        else if (rva >= rf.EndAddress) lo = mid + 1;
        else return &rf;
    }
    return nullptr;
}

void build_function_table() {
    if (!g_raw_pdata) return;
    const auto* rf = reinterpret_cast<const RUNTIME_FUNCTION*>(g_raw_pdata);
    for (std::uint32_t i = 0; i < g_raw_pdata_count; ++i) g_functions.add(rf[i].BeginAddress, rf[i].EndAddress);
    g_functions.finalize();
}

// ---- stack sampling ---------------------------------------------------------------------------
constexpr std::size_t kMaxDepth = 32;
constexpr std::uint32_t kNonCk3 = 0xFFFFFFFFu;
constexpr std::size_t kBurstCap = 20000;

// Capture one stack of a SUSPENDED thread (context already fetched). frames[0] = leaf function
// begin RVA in ck3.exe (or kNonCk3). Returns depth.
std::size_t unwind(CONTEXT& ctx, std::uint32_t* frames) {
    std::size_t depth = 0;
    for (; depth < kMaxDepth; ) {
        const std::uintptr_t rip = static_cast<std::uintptr_t>(ctx.Rip);
        if (rip == 0) break;
        const ModuleTable* m = module_for(rip);
        if (!m) { frames[depth++] = kNonCk3; g_modules_stale.store(true, std::memory_order_relaxed); break; }
        if (m->base == g_module_base) {
            const std::uint32_t rva = static_cast<std::uint32_t>(rip - g_module_base);
            const std::uint32_t fn = g_functions.lookup(rva);
            frames[depth++] = fn ? fn : rva;
        } else {
            frames[depth++] = kNonCk3;
        }
        const RUNTIME_FUNCTION* rf = find_rf(m, rip);
        if (!rf) {
            // leaf function without unwind info: return address is at [rsp]
            __try {
                ctx.Rip = *reinterpret_cast<const std::uint64_t*>(ctx.Rsp);
                ctx.Rsp += 8;
            } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
            continue;
        }
        void* handler_data = nullptr;
        ULONG64 establisher = 0;
        __try {
            ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, m->base, rip,
                               const_cast<RUNTIME_FUNCTION*>(rf), &ctx, &handler_data,
                               &establisher, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
    }
    return depth;
}

struct BurstResult {
    ck3accel::SampleHistogram self;
    ck3accel::SampleHistogram inclusive;
    ck3accel::StackTable<8192, kMaxDepth>* stacks = nullptr;
    std::size_t samples = 0;
    std::size_t truncated = 0;
};

std::atomic<bool> g_burst_active{false};
std::atomic<DWORD> g_tick_tid{0};
std::atomic<bool>  g_in_tick{false};
std::atomic<unsigned long long> g_tick_deadline_qpc{0};
std::atomic<std::uint64_t> g_day_index{0};
std::atomic<DWORD> g_main_tid{0};
std::atomic<unsigned long long> g_last_pump_qpc{0};
std::atomic<unsigned long long> g_pump_count{0};
constexpr unsigned long long kPumpGate = 3000;   // ~30-50 s of normal frames after the load screen
inline bool game_ready() { return g_pump_count.load(std::memory_order_relaxed) >= kPumpGate; }
long long g_qpc_freq = 1;
double g_qpc_to_ms = 1.0;

FILE* g_stacks_file = nullptr;
std::mutex g_stacks_mtx;
std::wstring self_dll_directory();

void write_burst(const char* kind, unsigned long long day, double dur_ms, const BurstResult& r) {
    std::lock_guard<std::mutex> lk(g_stacks_mtx);
    if (!g_stacks_file) {
        const std::wstring install = self_dll_directory();
        if (install.empty()) return;
        const std::wstring logs = install + L"\\logs";
        ::CreateDirectoryW(logs.c_str(), nullptr);
        const std::wstring path = logs + L"\\attrib_stacks.txt";
        g_stacks_file = _wfsopen(path.c_str(), L"a", _SH_DENYNO);   // shareable
        if (!g_stacks_file) return;
    }
    FILE* f = g_stacks_file;
    std::fprintf(f, "\n=== burst kind=%s day=%llu duration_ms=%.0f samples=%zu truncated=%zu ===\n",
                 kind, day, dur_ms, r.samples, r.truncated);
    std::fprintf(f, "-- self (leaf function) --\n");
    for (const auto& e : r.self.top_n(30))
        std::fprintf(f, "  rva=0x%08X samples=%llu pct=%.1f\n", e.rva, (unsigned long long)e.count,
                     r.samples ? 100.0 * (double)e.count / (double)r.samples : 0.0);
    std::fprintf(f, "-- inclusive (function anywhere on the stack) --\n");
    for (const auto& e : r.inclusive.top_n(40))
        std::fprintf(f, "  rva=0x%08X samples=%llu pct=%.1f\n", e.rva, (unsigned long long)e.count,
                     r.samples ? 100.0 * (double)e.count / (double)r.samples : 0.0);
    std::fprintf(f, "-- top stacks (leaf -> root) --\n");
    if (r.stacks) {
        for (const auto& s : r.stacks->top_n(20)) {
            std::fprintf(f, "  x%llu:", (unsigned long long)s.count);
            for (std::size_t i = 0; i < s.depth; ++i) std::fprintf(f, " %08X", s.frames[i]);
            std::fprintf(f, "\n");
        }
        std::fprintf(f, "  (distinct stacks=%zu overflow=%llu)\n", r.stacks->distinct(),
                     (unsigned long long)r.stacks->overflow());
    }
    std::fflush(f);
}

// Sample the thread `tid` until `keep_going` returns false or the cap is hit.
template <class Cond>
void burst(DWORD tid, const char* kind, Cond keep_going) {
    if (g_burst_active.exchange(true)) return;
    HANDLE th = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) { g_burst_active.store(false); return; }
    static ck3accel::StackTable<8192, kMaxDepth>* stacks = new ck3accel::StackTable<8192, kMaxDepth>();
    stacks->reset();
    BurstResult r; r.stacks = stacks;
    static std::uint32_t frames[kMaxDepth];
    static CONTEXT ctx;
    LARGE_INTEGER t0; ::QueryPerformanceCounter(&t0);
    const unsigned long long day = g_day_index.load(std::memory_order_relaxed);
    while (r.samples < kBurstCap && keep_going()) {
        if (g_host && g_host->is_kill_switch_active && g_host->is_kill_switch_active() != 0) break;
        if (::SuspendThread(th) == static_cast<DWORD>(-1)) break;
        std::memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        std::size_t depth = 0;
        if (::GetThreadContext(th, &ctx)) depth = unwind(ctx, frames);
        ::ResumeThread(th);
        if (depth == 0) { ::Sleep(1); continue; }
        ++r.samples;
        if (depth == kMaxDepth) ++r.truncated;
        r.self.add(frames[0]);
        // inclusive: each distinct function once per sample
        for (std::size_t i = 0; i < depth; ++i) {
            bool seen = false;
            for (std::size_t j = 0; j < i; ++j) if (frames[j] == frames[i]) { seen = true; break; }
            if (!seen) r.inclusive.add(frames[i]);
        }
        stacks->record(frames, depth);
        ::Sleep(1);
    }
    ::CloseHandle(th);
    LARGE_INTEGER t1; ::QueryPerformanceCounter(&t1);
    const double dur_ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * g_qpc_to_ms;
    write_burst(kind, day, dur_ms, r);
    if (g_host && g_host->log) {
        char msg[192];
        std::snprintf(msg, sizeof(msg), "accel_attrib_probe: %s burst: %zu stack samples over %.0f ms; top self:",
                      kind, r.samples, dur_ms);
        g_host->log(kLogWarn, msg);
        for (const auto& e : r.self.top_n(6)) {
            char line[96];
            std::snprintf(line, sizeof(line), "   rva=0x%08X samples=%llu", e.rva, (unsigned long long)e.count);
            g_host->log(kLogInfo, line);
        }
    }
    g_burst_active.store(false);
}

// ---- message pump hook (UI stall detection on the main thread) --------------------------------
using peek_fn = BOOL (WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using getmsg_fn = BOOL (WINAPI*)(LPMSG, HWND, UINT, UINT);
peek_fn   g_orig_peek = nullptr;
getmsg_fn g_orig_getmsg = nullptr;

inline void note_pump() {
    if (::GetCurrentThreadId() == g_main_tid.load(std::memory_order_relaxed)) {
        LARGE_INTEGER now; ::QueryPerformanceCounter(&now);
        g_last_pump_qpc.store(static_cast<unsigned long long>(now.QuadPart), std::memory_order_relaxed);
        g_pump_count.fetch_add(1, std::memory_order_relaxed);
    }
}
BOOL WINAPI detour_peek(LPMSG m, HWND h, UINT a, UINT b, UINT c) { note_pump(); return g_orig_peek(m, h, a, b, c); }
BOOL WINAPI detour_getmsg(LPMSG m, HWND h, UINT a, UINT b) { note_pump(); return g_orig_getmsg(m, h, a, b); }

DWORD find_main_thread() {
    // The process's first-created thread ran main(); Clausewitz runs game logic + GUI on it.
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = ::GetCurrentProcessId();
    THREADENTRY32 te; te.dwSize = sizeof(te);
    DWORD best = 0; unsigned long long best_ct = ~0ull;
    if (::Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE th = ::OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) continue;
            FILETIME c, e, k, u;
            if (::GetThreadTimes(th, &c, &e, &k, &u)) {
                const unsigned long long ct = (static_cast<unsigned long long>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
                if (ct < best_ct) { best_ct = ct; best = te.th32ThreadID; }
            }
            ::CloseHandle(th);
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);
    return best;
}

// ---- per-day node dump ------------------------------------------------------------------------
using tick_fn = int (*)(void* gamestate);
tick_fn g_orig_tick = nullptr;
FILE* g_nodes_csv = nullptr;
FILE* g_repeat_csv = nullptr;
ck3accel::NodeTable<kNodeCap>* g_merge_nodes = nullptr;
ck3accel::RepeatTable<kRepeatCap, kRepeatProbe>* g_merge_repeat = nullptr;

std::wstring self_dll_directory() {
    HMODULE self = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(&self_dll_directory), &self)) return std::wstring();
    std::wstring buf; buf.resize(MAX_PATH);
    for (;;) {
        DWORD len = ::GetModuleFileNameW(self, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) return std::wstring();
        if (len < buf.size()) { buf.resize(len); break; }
        buf.resize(buf.size() * 2);
    }
    const auto last = buf.find_last_of(L"\\/");
    if (last == std::wstring::npos) return std::wstring();
    const std::wstring plugins_dir = buf.substr(0, last);
    const auto prev = plugins_dir.find_last_of(L"\\/");
    return prev == std::wstring::npos ? plugins_dir : plugins_dir.substr(0, prev);
}

FILE* open_log(const wchar_t* name, const char* header) {
    const std::wstring install = self_dll_directory();
    if (install.empty()) return nullptr;
    const std::wstring logs = install + L"\\logs";
    ::CreateDirectoryW(logs.c_str(), nullptr);
    const std::wstring path = logs + L"\\" + name;
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    const bool existed = (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    FILE* f = nullptr;
    f = _wfsopen(path.c_str(), L"a", _SH_DENYNO);   // shareable: readable while the game runs
    if (!f) return nullptr;
    if (!existed && header) { std::fputs(header, f); std::fflush(f); }
    return f;
}

void dump_nodes(unsigned long long day, double wall_ms) {
    g_merge_nodes->reset();
    if (g_merge_repeat) g_merge_repeat->reset();
    {
        std::lock_guard<std::mutex> lk(g_shards_mtx);
        for (Shard* sh : g_shards) {
            g_merge_nodes->merge_from(sh->nodes);
            sh->nodes.reset();
            if (sh->repeat && g_merge_repeat) { g_merge_repeat->merge_from(*sh->repeat); sh->repeat->reset(); }
        }
    }
    const unsigned long long evals = g_merge_nodes->total_records();
    if (g_merge_repeat && g_repeat_csv) {
        const ck3accel::RepeatTally t = g_merge_repeat->tally();
        const double rep = t.total ? 100.0 * (double)(t.total - t.distinct) / (double)t.total : 0.0;
        const double inc = t.repeated_keys ? 100.0 * (double)t.inconsistent_keys / (double)t.repeated_keys : 0.0;
        std::fprintf(g_repeat_csv, "%llu,%.1f,%llu,%llu,%.3f,%llu,%.4f,%llu\n", day, wall_ms,
                     (unsigned long long)t.total, (unsigned long long)t.distinct, rep,
                     (unsigned long long)t.inconsistent_keys, inc, (unsigned long long)t.overflow);
        std::fflush(g_repeat_csv);
    }
    const bool big = evals >= static_cast<unsigned long long>(g_cfg.dump_min_evals) ||
                     wall_ms >= static_cast<double>(g_cfg.freeze_threshold_ms);
    if (!big || !g_nodes_csv) return;
    for (const auto& e : g_merge_nodes->top_n(static_cast<std::size_t>(g_cfg.top_nodes))) {
        char name[256] = "?"; std::uint32_t id = 0;
        node_class_name(reinterpret_cast<const void*>(e.key), name, sizeof(name), &id);
        std::fprintf(g_nodes_csv, "%llu,%.1f,%llu,0x%llX,%llu,%llu,%u,\"%s\"\n", day, wall_ms, evals,
                     (unsigned long long)e.key, (unsigned long long)e.count, (unsigned long long)e.cycles, id, name);
    }
    std::fflush(g_nodes_csv);
    if (g_host && g_host->log) {
        char msg[192];
        std::snprintf(msg, sizeof(msg),
            "accel_attrib_probe: day %llu: %llu trigger evals, %zu distinct nodes (overflow %llu); top nodes -> attrib_nodes.csv",
            day, evals, g_merge_nodes->distinct(), (unsigned long long)g_merge_nodes->overflow());
        g_host->log(kLogWarn, msg);
        for (const auto& e : g_merge_nodes->top_n(5)) {
            char name[160] = "?"; std::uint32_t id = 0;
            node_class_name(reinterpret_cast<const void*>(e.key), name, sizeof(name), &id);
            char line[256];
            std::snprintf(line, sizeof(line), "   node=0x%llX evals=%llu id=%u %s",
                          (unsigned long long)e.key, (unsigned long long)e.count, id, name);
            g_host->log(kLogInfo, line);
        }
    }
}

int detour_tick(void* gamestate) {
    g_tick_tid.store(::GetCurrentThreadId(), std::memory_order_release);
    LARGE_INTEGER t0; ::QueryPerformanceCounter(&t0);
    g_tick_deadline_qpc.store(static_cast<unsigned long long>(t0.QuadPart) +
        static_cast<unsigned long long>(static_cast<double>(g_cfg.freeze_threshold_ms) / g_qpc_to_ms),
        std::memory_order_release);
    g_in_tick.store(true, std::memory_order_release);
    const int rc = g_orig_tick(gamestate);
    g_in_tick.store(false, std::memory_order_release);
    LARGE_INTEGER t1; ::QueryPerformanceCounter(&t1);
    const double wall_ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * g_qpc_to_ms;
    const unsigned long long day = g_day_index.fetch_add(1, std::memory_order_relaxed);
    if (g_cfg.nodes) dump_nodes(day, wall_ms);
    return rc;
}

DWORD WINAPI watchdog_main(LPVOID) {
    ::timeBeginPeriod(1);
    map_modules();   // this thread is the only reader of g_modules (unwinder) and refreshes it itself
    LARGE_INTEGER start; ::QueryPerformanceCounter(&start);
    LARGE_INTEGER last_map = start;
    bool did_late_map = false;
    for (;;) {
        ::Sleep(2);
        LARGE_INTEGER now; ::QueryPerformanceCounter(&now);
        const unsigned long long q = static_cast<unsigned long long>(now.QuadPart);
        // Module table refresh: once at +60 s (late-loaded renderer/audio DLLs), then only when a
        // burst met an unmapped module, at most every 5 min. (A periodic 30 s refresh caused a
        // visible ~150 ms main-thread hitch every 30 s in the first live run.)
        const double since_map_ms = static_cast<double>(now.QuadPart - last_map.QuadPart) * g_qpc_to_ms;
        const bool late = !did_late_map && static_cast<double>(now.QuadPart - start.QuadPart) * g_qpc_to_ms > 60000.0;
        if (!g_burst_active.load(std::memory_order_relaxed) &&
            (late || (g_modules_stale.load(std::memory_order_relaxed) && since_map_ms > 300000.0))) {
            map_modules();
            g_modules_stale.store(false, std::memory_order_relaxed);
            last_map = now;
            if (late) did_late_map = true;
        }
        if (!game_ready()) continue;   // still loading: no sampling at all
        if (g_cfg.freeze_stacks && g_in_tick.load(std::memory_order_acquire)) {
            const unsigned long long dl = g_tick_deadline_qpc.load(std::memory_order_acquire);
            if (dl != 0 && q >= dl) {
                burst(g_tick_tid.load(std::memory_order_acquire), "day",
                      [] { return g_in_tick.load(std::memory_order_acquire); });
                continue;
            }
        }
        if (g_cfg.ui_stacks) {
            const DWORD tid = g_main_tid.load(std::memory_order_relaxed);
            const unsigned long long last = g_last_pump_qpc.load(std::memory_order_relaxed);
            if (tid != 0 && last != 0 &&
                (q - last) * g_qpc_to_ms >= static_cast<double>(g_cfg.ui_threshold_ms) &&
                !g_in_tick.load(std::memory_order_acquire)) {
                const unsigned long long start_last = last;
                burst(tid, "ui", [start_last] {
                    return g_last_pump_qpc.load(std::memory_order_relaxed) == start_last;
                });
            }
        }
    }
}

void read_conf() {
    const std::wstring install = self_dll_directory();
    if (install.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (install + L"\\attrib_probe.conf").c_str(), L"rb") != 0 || !f) return;
    char buf[1024]; std::string txt; std::size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) txt.append(buf, n);
    std::fclose(f);
    std::size_t pos = 0;
    auto trim = [](std::string s) { const char* w = " \t\r\n"; auto a = s.find_first_not_of(w);
        if (a == std::string::npos) return std::string(); auto b = s.find_last_not_of(w); return s.substr(a, b - a + 1); };
    auto truthy = [](const std::string& v) { return v == "1" || v == "true" || v == "yes" || v == "on"; };
    while (pos <= txt.size()) {
        const std::size_t nl = txt.find('\n', pos);
        std::string line = txt.substr(pos, (nl == std::string::npos ? txt.size() : nl) - pos);
        pos = (nl == std::string::npos) ? txt.size() + 1 : nl + 1;
        const std::size_t h = line.find('#'); if (h != std::string::npos) line = line.substr(0, h);
        const std::size_t eq = line.find('='); if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "attrib_probe") g_cfg.enabled = truthy(v);
        else if (k == "nodes") g_cfg.nodes = truthy(v);
        else if (k == "repeat") g_cfg.repeat = truthy(v);
        else if (k == "freeze_stacks") g_cfg.freeze_stacks = truthy(v);
        else if (k == "ui_stacks") g_cfg.ui_stacks = truthy(v);
        else if (k == "freeze_threshold_ms") g_cfg.freeze_threshold_ms = std::atoi(v.c_str());
        else if (k == "ui_threshold_ms") g_cfg.ui_threshold_ms = std::atoi(v.c_str());
        else if (k == "top_nodes") g_cfg.top_nodes = std::atoi(v.c_str());
        else if (k == "dump_min_evals") g_cfg.dump_min_evals = std::atoll(v.c_str());
    }
}

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)),
    CK3ACCEL_PLUGIN_MAGIC,
    CK3ACCEL_ABI_VERSION,
    "accel_attrib_probe",
    "0.1.0",
    "any",
    "any",
    CK3ACCEL_MODE_SP,
};
}  // namespace

PLUGIN_EXPORT const CK3AccelPluginInfo* CK3Accel_Query(uint32_t host_abi_version) {
    (void)host_abi_version;
    return &kInfo;
}

PLUGIN_EXPORT int CK3Accel_Init(const CoreApi* host, CK3AccelRegistrar* reg) {
    g_host = host;
    if (!host || !host->log || !host->scan || !host->install_hook || !reg) return 1;
    g_module_base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    {
        MODULEINFO mi{};
        if (::GetModuleInformation(::GetCurrentProcess(), ::GetModuleHandleW(nullptr), &mi, sizeof(mi)))
            g_image_size = mi.SizeOfImage;
    }
    LARGE_INTEGER freq;
    if (::QueryPerformanceFrequency(&freq) && freq.QuadPart != 0) {
        g_qpc_freq = freq.QuadPart;
        g_qpc_to_ms = 1.0e3 / static_cast<double>(freq.QuadPart);
    }
    read_conf();
    if (!g_cfg.enabled) {
        host->log(kLogInfo, "accel_attrib_probe: disabled (set attrib_probe=true in attrib_probe.conf); inert");
        return 0;
    }
    map_modules();
    build_function_table();
    if (g_functions.size() == 0) {
        host->log(kLogWarn, "accel_attrib_probe: could not read ck3.exe .pdata; stack bursts disabled");
        g_cfg.freeze_stacks = g_cfg.ui_stacks = false;
    }

    void* trig = host->scan(kSigTrigger);
    void* tick = host->scan(kSigTick);
    if (!tick) {
        host->log(kLogWarn, "accel_attrib_probe: UpdateTurnTick signature NOT FOUND; probe inert");
        return 0;
    }
    if (g_cfg.nodes) {
        if (!trig) {
            host->log(kLogWarn, "accel_attrib_probe: trigger signature NOT FOUND; node attribution off");
            g_cfg.nodes = false;
        } else {
            g_merge_nodes = new ck3accel::NodeTable<kNodeCap>();
            if (g_cfg.repeat) g_merge_repeat = new ck3accel::RepeatTable<kRepeatCap, kRepeatProbe>();
            g_nodes_csv = open_log(L"attrib_nodes.csv", "day_index,wall_ms,day_evals,node,evals,cycles,trigger_id,class\n");
            if (g_cfg.repeat) g_repeat_csv = open_log(L"attrib_repeat.csv",
                "day_index,wall_ms,total_evals,distinct_node_entity_keys,repeat_pct,inconsistent_keys,inconsistency_pct,overflow\n");
            host->install_hook(reg->hook_set, trig, reinterpret_cast<void*>(&detour_trigger),
                               reinterpret_cast<void**>(&g_orig_trigger));
            host->log(kLogInfo, "accel_attrib_probe: hooked trigger evaluator (node attribution + (node,entity) repeat)");
        }
    }
    host->install_hook(reg->hook_set, tick, reinterpret_cast<void*>(&detour_tick),
                       reinterpret_cast<void**>(&g_orig_tick));
    host->log(kLogInfo, "accel_attrib_probe: hooked UpdateTurnTick");

    {
        g_main_tid.store(find_main_thread(), std::memory_order_relaxed);
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        void* peek = user32 ? reinterpret_cast<void*>(::GetProcAddress(user32, "PeekMessageW")) : nullptr;
        void* getm = user32 ? reinterpret_cast<void*>(::GetProcAddress(user32, "GetMessageW")) : nullptr;
        if (peek) host->install_hook(reg->hook_set, peek, reinterpret_cast<void*>(&detour_peek),
                                     reinterpret_cast<void**>(&g_orig_peek));
        if (getm) host->install_hook(reg->hook_set, getm, reinterpret_cast<void*>(&detour_getmsg),
                                     reinterpret_cast<void**>(&g_orig_getmsg));
        char msg[160];
        std::snprintf(msg, sizeof(msg), "accel_attrib_probe: UI stall watchdog on main thread %lu (pump hooks %s/%s)",
                      static_cast<unsigned long>(g_main_tid.load()), peek ? "PeekMessageW" : "-", getm ? "GetMessageW" : "-");
        host->log(kLogInfo, msg);
    }
    if (HANDLE th = ::CreateThread(nullptr, 0, &watchdog_main, nullptr, 0, nullptr)) ::CloseHandle(th);
    host->log(kLogInfo, "accel_attrib_probe: active");
    return 0;
}
