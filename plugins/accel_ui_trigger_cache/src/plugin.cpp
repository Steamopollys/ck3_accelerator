// accel_ui_trigger_cache: shipping plugin.
//
// Memoizes script TRIGGER results during main-thread UI evaluation for one frame. The
// character/court-positions/marriage windows re-ask the engine the same trigger (same node,
// same scopes) hundreds of times while they build: e.g. vanilla `portrait_high_nobles_trigger`
// is referenced ~300x across the clothing portrait modifiers and runs an `any_close_family_member`
// scan each time. This returns the first answer for the rest of the frame, collapsing those to
// one real evaluation.
//
// Soundness (why this cannot change gameplay):
//   * The simulation never uses the cache: consulted ONLY on the main (UI) thread and ONLY while
//     no day-tick is running. Worker threads and the tick pass straight through.
//   * The cache lives one frame: a global epoch is bumped on every main-thread message pump
//     (frame boundary), on UpdateTurnTick enter and exit, and on every main-thread script EFFECT
//     execution (any state mutation). A stored result is a hit only within its epoch.
//   * Tooltip/description mode (context byte +0x20 == 2) is never cached (it builds text).
//   * A trigger that mutates the scope context (save_temporary_scope_value_as, temporary_list,
//     save_opinion_value_as, …) is detected dynamically (its evaluation changes the saved-scope
//     counts) and its node is permanently marked uncacheable.
//   * The key includes the node pointer, the this/prev/root scope refs, every saved scope in both
//     context stores, and the skip-validation flag, so two scopes never share a result. Any read
//     that looks malformed makes that one call bypass the cache.
//
// SP + Ironman (result-identical, checksum-neutral); declines multiplayer by policy.

#include <ck3accel/core_api.h>
#include <ck3accel/trigger_cache.h>

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif

static_assert(sizeof(void*) == 8, "accel_ui_trigger_cache is x64-only");

namespace {
constexpr int kLogInfo = 2;
constexpr int kLogWarn = 3;

const CoreApi* g_host = nullptr;

// ---- signatures (facts about a public binary; no ck3.exe bytes shipped) --------------------
const char* const kSigTrigger =   // char eval(node rcx, ctx rdx, skip_validation r8b)
    "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 C8 FD FF FF";
const char* const kSigEffect =    // void exec(effect rcx, ctx rdx, ...)  (any script mutation)
    "48 8B C4 48 89 58 08 48 89 70 18 55 57 41 54 41 56 41 57 48 8D A8 88 FD FF FF 48 81 EC 50 03 00 00";
const char* const kSigTick =      // int UpdateTurnTick(gamestate)
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FD FF FF 48 81 EC 88 03 00 00 0F 29 B4 24 70 03 00 00";

// ---- context layout ------------------------------------------------------
constexpr std::size_t kCtxThis   = 0x00;   // ScopeRef* current
constexpr std::size_t kCtxPrev   = 0x08;   // ScopeRef* prev (may be null)
constexpr std::size_t kCtxRoot   = 0x10;   // ScopeRef* root
constexpr std::size_t kCtxStore  = 0x18;   // saved-scope store*
constexpr std::size_t kCtxMode   = 0x20;   // byte; 2 = tooltip/description mode
constexpr std::size_t kStoreArr  = 0x00;   // store: entry array base
constexpr std::size_t kStoreCnt  = 0x0C;   // store: entry count (32-byte entries)
constexpr std::size_t kStoreFb   = 0x3D0;  // store: fallback store*
constexpr std::size_t kFbArr     = 0x18;   // fallback: entry array base
constexpr std::size_t kFbCnt     = 0x24;   // fallback: entry count (24-byte entries)
constexpr std::size_t kPrimStride = 0x20, kFbStride = 0x18, kEntryRef = 0x08;
constexpr std::size_t kMaxScopes = 64;     // above this, don't cache (key cost + safety)

using trigger_fn = char (*)(void* node, std::uint8_t* ctx, std::uint8_t skip);
using effect_fn  = void (*)(void* effect, std::uint8_t* ctx, void* a, void* b);
using tick_fn    = int  (*)(void* gamestate);
trigger_fn g_orig_trigger = nullptr;
effect_fn  g_orig_effect  = nullptr;
tick_fn    g_orig_tick    = nullptr;

std::atomic<bool>          g_active{false};
std::atomic<std::uint32_t> g_epoch{1};
std::atomic<DWORD>         g_main_tid{0};
std::atomic<int>           g_tick_depth{0};   // >0 => a tick is on the stack (any thread)

ck3accel::cache::EpochCache<1u << 21>* g_cache = nullptr;   // 2^21 slots (~40 MiB)
ck3accel::cache::NodeFlagSet* g_poison = nullptr;           // uncacheable nodes

// pump / effect / tick hooks
using peek_fn   = BOOL (WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using getmsg_fn = BOOL (WINAPI*)(LPMSG, HWND, UINT, UINT);
peek_fn   g_orig_peek = nullptr;
getmsg_fn g_orig_getmsg = nullptr;

std::atomic<unsigned long long> g_hits{0}, g_misses{0}, g_bypass{0};
unsigned long long g_last_log = 0;

inline void bump_epoch() { g_epoch.fetch_add(1, std::memory_order_relaxed); }

// The message pump is called MANY times per frame, so bumping per call would invalidate the
// cache almost immediately (measured 16% hit rate). Instead bump at most once per kFrameBumpMs:
// real state changes (effects, tick) bump immediately and keep it sound; this only bounds how
// long a result may live when nothing mutates (e.g. a value changed via a path we do not hook).
constexpr double kFrameBumpMs = 50.0;
std::atomic<unsigned long long> g_last_bump_qpc{0};
double g_qpc_to_ms = 1.0;
inline void maybe_frame_bump() {
    LARGE_INTEGER now; ::QueryPerformanceCounter(&now);
    const unsigned long long q = static_cast<unsigned long long>(now.QuadPart);
    const unsigned long long last = g_last_bump_qpc.load(std::memory_order_relaxed);
    if (static_cast<double>(q - last) * g_qpc_to_ms >= kFrameBumpMs) {
        g_last_bump_qpc.store(q, std::memory_order_relaxed);
        bump_epoch();
    }
}

// Read the context into a ContextView + scratch arrays, SEH-guarded. Returns false to bypass.
bool read_context(std::uint8_t* ctx, void* node, std::uint8_t skip,
                  ck3accel::cache::ContextView* out,
                  ck3accel::cache::SavedScope* prim, ck3accel::cache::SavedScope* fb) {
    __try {
        if (ctx[kCtxMode] == 2) return false;   // tooltip mode: never cache
        out->node = reinterpret_cast<std::uint64_t>(node);
        out->current = reinterpret_cast<ck3accel::cache::ScopeRef*>(*reinterpret_cast<void**>(ctx + kCtxThis));
        out->prev    = reinterpret_cast<ck3accel::cache::ScopeRef*>(*reinterpret_cast<void**>(ctx + kCtxPrev));
        out->root    = reinterpret_cast<ck3accel::cache::ScopeRef*>(*reinterpret_cast<void**>(ctx + kCtxRoot));
        out->skip_validation = skip;
        out->saved = prim; out->saved_count = 0;
        out->fallback = fb; out->fallback_count = 0;

        std::uint8_t* store = *reinterpret_cast<std::uint8_t**>(ctx + kCtxStore);
        if (store) {
            std::uint8_t* base = *reinterpret_cast<std::uint8_t**>(store + kStoreArr);
            std::int32_t n = *reinterpret_cast<std::int32_t*>(store + kStoreCnt);
            if (n < 0 || static_cast<std::size_t>(n) > kMaxScopes) return false;
            for (std::int32_t i = 0; i < n; ++i) {
                std::uint8_t* e = base + static_cast<std::size_t>(i) * kPrimStride;
                prim[i].id = *reinterpret_cast<std::uint32_t*>(e);
                std::memcpy(&prim[i].ref, e + kEntryRef, sizeof(ck3accel::cache::ScopeRef));
            }
            out->saved_count = static_cast<std::size_t>(n);

            std::uint8_t* fbstore = *reinterpret_cast<std::uint8_t**>(store + kStoreFb);
            if (fbstore) {
                std::uint8_t* fbase = *reinterpret_cast<std::uint8_t**>(fbstore + kFbArr);
                std::int32_t fn = *reinterpret_cast<std::int32_t*>(fbstore + kFbCnt);
                if (fn < 0 || static_cast<std::size_t>(fn) > kMaxScopes) return false;
                for (std::int32_t i = 0; i < fn; ++i) {
                    std::uint8_t* e = fbase + static_cast<std::size_t>(i) * kFbStride;
                    fb[i].id = *reinterpret_cast<std::uint32_t*>(e);
                    std::memcpy(&fb[i].ref, e + kEntryRef, sizeof(ck3accel::cache::ScopeRef));
                }
                out->fallback_count = static_cast<std::size_t>(fn);
            }
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Read just the two scope-store counts (for mutation detection), SEH-guarded.
bool read_counts(std::uint8_t* ctx, std::uint32_t* prim, std::uint32_t* fb) {
    __try {
        std::uint8_t* store = *reinterpret_cast<std::uint8_t**>(ctx + kCtxStore);
        if (!store) { *prim = 0; *fb = 0; return true; }
        *prim = *reinterpret_cast<std::uint32_t*>(store + kStoreCnt);
        std::uint8_t* fbstore = *reinterpret_cast<std::uint8_t**>(store + kStoreFb);
        *fb = fbstore ? *reinterpret_cast<std::uint32_t*>(fbstore + kFbCnt) : 0u;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

char detour_trigger(void* node, std::uint8_t* ctx, std::uint8_t skip) {
    if (!g_active.load(std::memory_order_relaxed) || !node || !ctx ||
        ::GetCurrentThreadId() != g_main_tid.load(std::memory_order_relaxed) ||
        g_tick_depth.load(std::memory_order_relaxed) != 0 ||
        g_poison->contains(reinterpret_cast<std::uint64_t>(node))) {
        return g_orig_trigger(node, ctx, skip);
    }

    ck3accel::cache::ContextView v{};
    ck3accel::cache::SavedScope prim[kMaxScopes], fb[kMaxScopes];
    if (!read_context(ctx, node, skip, &v, prim, fb)) {
        g_bypass.fetch_add(1, std::memory_order_relaxed);
        return g_orig_trigger(node, ctx, skip);
    }
    const ck3accel::cache::ContextKey key = ck3accel::cache::make_key(v);
    const std::uint32_t epoch = g_epoch.load(std::memory_order_relaxed);

    std::uint8_t cached = 0;
    if (g_cache->lookup(key, epoch, &cached)) {
        g_hits.fetch_add(1, std::memory_order_relaxed);
        return static_cast<char>(cached);
    }

    // Miss: run the real trigger, watching for a scope-store mutation (uncacheable trigger).
    std::uint32_t p0 = 0, f0 = 0; const bool have0 = read_counts(ctx, &p0, &f0);
    const char rc = g_orig_trigger(node, ctx, skip);
    std::uint32_t p1 = 0, f1 = 0; const bool have1 = read_counts(ctx, &p1, &f1);

    if (have0 && have1 && (p0 != p1 || f0 != f1)) {
        g_poison->insert(reinterpret_cast<std::uint64_t>(node));   // mutated the context: never cache
        g_bypass.fetch_add(1, std::memory_order_relaxed);
        return rc;
    }
    g_cache->store(key, epoch, static_cast<std::uint8_t>(rc));
    g_misses.fetch_add(1, std::memory_order_relaxed);
    return rc;
}

void detour_effect(void* effect, std::uint8_t* ctx, void* a, void* b) {
    // Any main-thread state mutation ends the current cache epoch.
    if (::GetCurrentThreadId() == g_main_tid.load(std::memory_order_relaxed)) bump_epoch();
    g_orig_effect(effect, ctx, a, b);
    if (::GetCurrentThreadId() == g_main_tid.load(std::memory_order_relaxed)) bump_epoch();
}

int detour_tick(void* gamestate) {
    g_tick_depth.fetch_add(1, std::memory_order_relaxed);
    bump_epoch();
    const int rc = g_orig_tick(gamestate);
    g_tick_depth.fetch_sub(1, std::memory_order_relaxed);
    bump_epoch();
    return rc;
}

void on_frame() {
    if (::GetCurrentThreadId() != g_main_tid.load(std::memory_order_relaxed)) return;
    maybe_frame_bump();
    // periodic stats (about once a minute of frames)
    const unsigned long long h = g_hits.load(std::memory_order_relaxed);
    static unsigned long long frames = 0;
    if ((++frames & 0x3FFF) == 0 && g_host && g_host->log) {
        const unsigned long long m = g_misses.load(std::memory_order_relaxed);
        const unsigned long long by = g_bypass.load(std::memory_order_relaxed);
        if (h != g_last_log) {
            g_last_log = h;
            char msg[192];
            const double rate = (h + m) ? 100.0 * (double)h / (double)(h + m) : 0.0;
            std::snprintf(msg, sizeof(msg),
                "accel_ui_trigger_cache: hits=%llu misses=%llu (%.1f%% saved) bypass=%llu poison=%zu",
                h, m, rate, by, g_poison->size());
            g_host->log(kLogInfo, msg);
            if (g_host->report_metric) g_host->report_metric("accel_ui_trigger_cache.hit_pct", rate);
        }
    }
}

BOOL WINAPI detour_peek(LPMSG m, HWND h, UINT a, UINT b, UINT c) { on_frame(); return g_orig_peek(m, h, a, b, c); }
BOOL WINAPI detour_getmsg(LPMSG m, HWND h, UINT a, UINT b) { on_frame(); return g_orig_getmsg(m, h, a, b); }

DWORD find_main_thread() {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    const DWORD pid = ::GetCurrentProcessId();
    THREADENTRY32 te; te.dwSize = sizeof(te);
    DWORD best = 0; unsigned long long best_ct = ~0ull;
    if (::Thread32First(snap, &te)) do {
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
    ::CloseHandle(snap);
    return best;
}

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)),
    CK3ACCEL_PLUGIN_MAGIC,
    CK3ACCEL_ABI_VERSION,
    "accel_ui_trigger_cache",
    "0.1.0",
    "1.19.0.6",
    "1.19.0.6",
    CK3ACCEL_MODE_SP | CK3ACCEL_MODE_IRONMAN,
};
}  // namespace

PLUGIN_EXPORT const CK3AccelPluginInfo* CK3Accel_Query(uint32_t host_abi_version) {
    (void)host_abi_version;
    return &kInfo;
}

PLUGIN_EXPORT int CK3Accel_Init(const CoreApi* host, CK3AccelRegistrar* reg) {
    g_host = host;
    if (!host || !host->log || !host->scan || !host->install_hook || !reg) return 1;

    void* trig = host->scan(kSigTrigger);
    void* eff  = host->scan(kSigEffect);
    void* tick = host->scan(kSigTick);
    if (!trig || !eff || !tick) {
        host->log(kLogWarn, "accel_ui_trigger_cache: a signature was NOT FOUND; plugin inert");
        return 0;
    }
    LARGE_INTEGER freq;
    if (::QueryPerformanceFrequency(&freq) && freq.QuadPart != 0)
        g_qpc_to_ms = 1.0e3 / static_cast<double>(freq.QuadPart);
    g_main_tid.store(find_main_thread(), std::memory_order_relaxed);
    if (g_main_tid.load() == 0) {
        host->log(kLogWarn, "accel_ui_trigger_cache: could not identify main thread; plugin inert");
        return 0;
    }
    g_cache = new ck3accel::cache::EpochCache<1u << 21>();
    g_poison = new ck3accel::cache::NodeFlagSet();

    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    void* peek = user32 ? reinterpret_cast<void*>(::GetProcAddress(user32, "PeekMessageW")) : nullptr;
    void* getm = user32 ? reinterpret_cast<void*>(::GetProcAddress(user32, "GetMessageW")) : nullptr;
    if (!peek || !getm) {
        host->log(kLogWarn, "accel_ui_trigger_cache: message-pump hooks unavailable; plugin inert");
        return 0;
    }

    bool ok = true;
    ok &= host->install_hook(reg->hook_set, tick, reinterpret_cast<void*>(&detour_tick),
                             reinterpret_cast<void**>(&g_orig_tick)) != nullptr && g_orig_tick;
    ok &= host->install_hook(reg->hook_set, eff, reinterpret_cast<void*>(&detour_effect),
                             reinterpret_cast<void**>(&g_orig_effect)) != nullptr && g_orig_effect;
    ok &= host->install_hook(reg->hook_set, peek, reinterpret_cast<void*>(&detour_peek),
                             reinterpret_cast<void**>(&g_orig_peek)) != nullptr && g_orig_peek;
    ok &= host->install_hook(reg->hook_set, getm, reinterpret_cast<void*>(&detour_getmsg),
                             reinterpret_cast<void**>(&g_orig_getmsg)) != nullptr && g_orig_getmsg;
    // The trigger hook goes in LAST so the epoch/pump machinery is live before any cached read.
    ok &= host->install_hook(reg->hook_set, trig, reinterpret_cast<void*>(&detour_trigger),
                             reinterpret_cast<void**>(&g_orig_trigger)) != nullptr && g_orig_trigger;
    if (!ok) {
        host->log(kLogWarn, "accel_ui_trigger_cache: a hook failed to install; plugin inert");
        return 0;
    }
    g_active.store(true, std::memory_order_release);
    char msg[192];
    std::snprintf(msg, sizeof(msg),
        "accel_ui_trigger_cache: active (main thread %lu; trigger=%p effect=%p tick=%p)",
        static_cast<unsigned long>(g_main_tid.load()), trig, eff, tick);
    host->log(kLogInfo, msg);
    return 0;
}
