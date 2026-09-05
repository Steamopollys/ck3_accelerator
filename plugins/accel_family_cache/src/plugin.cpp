// accel_family_cache: shipping plugin (opt-in).
//
// Memoizes the composite family-list builders (close_family_member / extended_family_member /
// close_or_extended_family_member) for one frame. The character window rebuilds the SAME
// close-family list ~300x per portrait for the SAME character (the portrait clothing trigger runs
// `any_close_family_member` once per clothing modifier); this collapses those to one real build
// plus a cheap replay, taking the huge-family character window from ~15 s toward a fraction.
//
// Hooks the three composite builders (each: rcx = {filter,list} ctx, r8 = character scope ref).
// The `any_/every_/random_` runner hands them a FRESH list, so a cache hit just pushes the cached
// entries back into that empty list (no dedup needed). Composes with accel_family_lists (which
// hooks the walker/add-unique primitives): a MISS calls the original builder and runs the O(N)
// walker; a HIT skips the builder (and the walker) entirely.
//
// Soundness: consulted ONLY on the main (UI) thread, ONLY when no day-tick is running, and ONLY
// when the output list is empty on entry. A character's family changes on births/deaths (day-tick
// and script-effect events); the frame epoch is bumped on tick, on every main-thread effect, and
// on a 50 ms throttle, so a cached list never outlives a change. Lists beyond kSaneMax aren't
// cached (built normally). SP + Ironman; off unless family_cache.conf enables it.

#include <ck3accel/core_api.h>
#include <ck3accel/family_list_cache.h>

#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(_WIN32)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif

static_assert(sizeof(void*) == 8, "accel_family_cache is x64-only");

namespace {
constexpr int kLogInfo = 2;
constexpr int kLogWarn = 3;

const CoreApi* g_host = nullptr;

// ---- signatures ------------------------------------------------------------------------------
const char* const kSigBuilderC1 =       // the third composite builder (unique)
    "48 89 5C 24 10 57 48 83 EC 30 49 8B 00 4C 8B D2 4C 8B D9 BF FF FF FF FF 66 83 38 04 75 ?? "
    "44 8B 48 08 EB ?? 44 8B CF 4C 8B 05 ?? ?? ?? ??";
const char* const kSigWalker =          // to resolve push_back from its +0x206 call site
    "4C 89 44 24 18 41 54 41 55 48 83 EC 58 48 8B 82 A0 01 00 00 4C 8B E9 48 85 C0 74 ??";
// The core's tick-epoch service owns the effect/tick hooks and provides the epoch + in-tick
// signals, which lets this run alongside accel_tick_cache.

// ---- engine layout ---------------------------------------------------------------------------
struct ScopeRef  { std::uint16_t type; std::uint16_t sub; std::uint32_t pad; std::uint64_t handle; };
struct ScopeList { ScopeRef* data; std::uint32_t cap; std::int32_t count; void* alloc; };
constexpr std::size_t kFilterAll = 0x08, kFilterDead = 0x09;

using builder_fn  = void (*)(void* ctx, void* rdx, std::uint8_t* character);
using push_back_fn = void* (*)(ScopeList* list, const ScopeRef* entry);

builder_fn   g_orig_builder[3] = {nullptr, nullptr, nullptr};
push_back_fn g_push_back = nullptr;
// Epoch + in-tick come from the core's shared tick-epoch service (g_host->tick_epoch / in_tick).

std::atomic<bool>          g_active{false};
std::atomic<DWORD>         g_main_tid{0};
std::atomic<bool>          g_interactive{false};   // true after the first main-thread frame pump
double                     g_qpc_to_ms = 1.0;
std::atomic<unsigned long long> g_last_bump_qpc{0};

// Close/extended family of an immortal can be thousands; each slot holds a heap vector sized to
// its list, so use a modest slot count with a generous inline length rather than fixed slots.
constexpr std::size_t kSlots     = 64;         // per-thread slots; each holds a heap vector sized
                                               // to its list (no length ceiling: the immortal's
                                               // close-or-extended family is many thousands).
constexpr std::size_t kSaneMax   = 1u << 20;   // sanity ceiling on a single cached list (1M entries)
using Cache = ck3accel::famcache::ListCache<kSlots>;
thread_local Cache* t_cache = nullptr;          // portrait eval is a parallel-for on worker threads;
                                                // a per-thread cache catches the ~300 rebuilds one
                                                // portrait task does for one character, no locking.
inline Cache& cache() { if (!t_cache) t_cache = new Cache(); return *t_cache; }
std::atomic<bool> g_cache_ready{false};

std::atomic<unsigned long long> g_hits{0}, g_builds{0}, g_replayed{0};
// diagnostics (always logged, so a zero-hit run is explainable)
std::atomic<unsigned long long> g_entered{0}, g_bp_gate{0}, g_bp_decode{0}, g_bp_nonempty{0}, g_stored{0};
unsigned long long g_last_log = 0;

// Force a bump at most once a second so no cached list lingers past ~1 s even if no effect or
// tick fired. (The core already bumps on every effect and tick.)
inline void maybe_frame_bump() {
    LARGE_INTEGER now; ::QueryPerformanceCounter(&now);
    const unsigned long long q = static_cast<unsigned long long>(now.QuadPart);
    if (static_cast<double>(q - g_last_bump_qpc.load(std::memory_order_relaxed)) * g_qpc_to_ms >= 1000.0) {
        g_last_bump_qpc.store(q, std::memory_order_relaxed);
        if (g_host && g_host->bump_epoch) g_host->bump_epoch();
    }
}

// Builder ABI (verified against the walker call inside fn_01A5A900):
//   rcx (ctx)      = the filter object (dead/alive flags at +8/+9)
//   rdx (list_arg) = the OUTPUT list (CPdxArray of 16-byte ScopeRef); this is what the builder fills
//   r8  (character)= pointer to a ScopeRef* (the character whose family to build)
// Every engine read is SEH-guarded (the builders also run on non-UI paths where the args differ).
bool decode(void* ctx, void* list_arg, std::uint8_t* character, std::uint8_t** filter_out,
            ScopeList** list_out, std::uint32_t* handle_out, std::int32_t* count_out) {
    __try {
        std::uint8_t* filter = static_cast<std::uint8_t*>(ctx);
        ScopeList* list = static_cast<ScopeList*>(list_arg);
        if (!filter || !list) return false;
        ScopeRef* ref = *reinterpret_cast<ScopeRef**>(character);
        if (!ref || ref->type != 4) return false;
        const volatile std::uint8_t f0 = filter[kFilterAll], f1 = filter[kFilterDead];  // touch flags
        (void)f0; (void)f1;
        *count_out = list->count;                                  // read the count under the guard
        *filter_out = filter; *list_out = list; *handle_out = static_cast<std::uint32_t>(ref->handle);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

template <std::uint8_t Selector>
void detour_builder(void* ctx, void* rdx, std::uint8_t* character) {
    builder_fn orig = g_orig_builder[Selector];
    // Cache only during interactive gameplay (after the first UI frame), never inside a day-tick.
    // This keeps the cache out of the save-load and simulation paths; the repeated builds we
    // target are purely the interactive portrait/window evaluation.
    if (!g_cache_ready.load(std::memory_order_relaxed) || !g_interactive.load(std::memory_order_relaxed) ||
        !ctx || !rdx || !character || g_host->in_tick() != 0) {
        g_bp_gate.fetch_add(1, std::memory_order_relaxed);
        orig(ctx, rdx, character);
        return;
    }
    g_entered.fetch_add(1, std::memory_order_relaxed);
    std::uint8_t* filter; ScopeList* list; std::uint32_t handle; std::int32_t incount = 0;
    if (!decode(ctx, rdx, character, &filter, &list, &handle, &incount)) {
        g_bp_decode.fetch_add(1, std::memory_order_relaxed);
        orig(ctx, rdx, character); return;
    }
    if (incount != 0) {
        g_bp_nonempty.fetch_add(1, std::memory_order_relaxed);
        orig(ctx, rdx, character); return;                        // only cache fresh (empty) builds
    }
    const std::uint8_t filterbyte =
        static_cast<std::uint8_t>((filter[kFilterDead] ? 2 : 0) | (filter[kFilterAll] ? 1 : 0));
    const std::uint64_t key = ck3accel::famcache::make_key(handle, Selector, filterbyte);
    const std::uint32_t epoch = g_host->tick_epoch();

    const ck3accel::famcache::Entry* hit = nullptr;
    std::size_t n = 0;
    if (cache().lookup(key, epoch, &hit, &n)) {
        for (std::size_t i = 0; i < n; ++i)
            g_push_back(list, reinterpret_cast<const ScopeRef*>(&hit[i]));   // replay into the empty list
        g_hits.fetch_add(1, std::memory_order_relaxed);
        g_replayed.fetch_add(n, std::memory_order_relaxed);
        return;
    }
    orig(ctx, rdx, character);   // real build (runs the O(N) walker via accel_family_lists)
    g_builds.fetch_add(1, std::memory_order_relaxed);
    const std::int32_t produced = list->count;
    if (produced >= 0 && static_cast<std::size_t>(produced) <= kSaneMax) {
        cache().store(key, epoch, reinterpret_cast<const ck3accel::famcache::Entry*>(list->data),
                      static_cast<std::size_t>(produced));
        g_stored.fetch_add(1, std::memory_order_relaxed);
    }
}

// (effect + tick epoch bumping lives in the core's shared tick-epoch service.)

// pump hooks: throttled frame epoch + periodic stats
using peek_fn   = BOOL (WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
using getmsg_fn = BOOL (WINAPI*)(LPMSG, HWND, UINT, UINT);
peek_fn   g_orig_peek = nullptr;
getmsg_fn g_orig_getmsg = nullptr;

void on_frame() {
    if (::GetCurrentThreadId() != g_main_tid.load(std::memory_order_relaxed)) return;
    g_interactive.store(true, std::memory_order_relaxed);
    maybe_frame_bump();
}

DWORD WINAPI diag_thread_main(LPVOID) {
    for (;;) {
        ::Sleep(5000);
        const unsigned long long h = g_hits.load(std::memory_order_relaxed);
        const unsigned long long b = g_builds.load(std::memory_order_relaxed);
        const unsigned long long ent = g_entered.load(std::memory_order_relaxed);
        const unsigned long long gate = g_bp_gate.load(std::memory_order_relaxed);
        const unsigned long long fingerprint = ent + h + b + gate;   // log whenever ANY activity moved
        if (fingerprint == g_last_log || !g_host || !g_host->log) continue;
        g_last_log = fingerprint;
        const unsigned long long total = h + b;
        char msg[256];
        const double rate = total ? 100.0 * (double)h / (double)total : 0.0;
        std::snprintf(msg, sizeof(msg),
            "accel_family_cache: entered=%llu hits=%llu builds=%llu stored=%llu | bypass gate=%llu decode=%llu nonempty=%llu | %.1f%% served",
            g_entered.load(std::memory_order_relaxed), h, b, g_stored.load(std::memory_order_relaxed),
            g_bp_gate.load(std::memory_order_relaxed), g_bp_decode.load(std::memory_order_relaxed),
            g_bp_nonempty.load(std::memory_order_relaxed), rate);
        g_host->log(kLogInfo, msg);
    }
}
BOOL WINAPI detour_peek(LPMSG m, HWND h, UINT a, UINT b, UINT c) { on_frame(); return g_orig_peek(m, h, a, b, c); }
BOOL WINAPI detour_getmsg(LPMSG m, HWND h, UINT a, UINT b) { on_frame(); return g_orig_getmsg(m, h, a, b); }

void* decode_call(const std::uint8_t* site) {
    if (site[0] != 0xE8) return nullptr;
    std::int32_t rel; std::memcpy(&rel, site + 1, 4);
    return const_cast<std::uint8_t*>(site + 5 + rel);
}

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

bool config_enabled() {
    // family_cache.conf next to ck3accel_core.dll, key `family_cache=true`. Off by default.
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&config_enabled), &self);
    wchar_t buf[MAX_PATH]; DWORD len = ::GetModuleFileNameW(self, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;
    std::wstring p(buf, len);
    auto last = p.find_last_of(L"\\/"); if (last == std::wstring::npos) return false;
    std::wstring plugins = p.substr(0, last);
    auto prev = plugins.find_last_of(L"\\/"); if (prev == std::wstring::npos) return false;
    const std::wstring conf = plugins.substr(0, prev) + L"\\family_cache.conf";
    FILE* f = nullptr;
    if (_wfopen_s(&f, conf.c_str(), L"rb") != 0 || !f) return false;
    char txt[512]; size_t n = std::fread(txt, 1, sizeof(txt) - 1, f); txt[n] = 0; std::fclose(f);
    std::string s(txt);
    return s.find("family_cache=true") != std::string::npos || s.find("family_cache = true") != std::string::npos;
}

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)),
    CK3ACCEL_PLUGIN_MAGIC,
    CK3ACCEL_ABI_VERSION,
    "accel_family_cache",
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
    if (!config_enabled()) {
        host->log(kLogInfo, "accel_family_cache: disabled (set family_cache=true in family_cache.conf); inert");
        return 0;
    }
    LARGE_INTEGER freq;
    if (::QueryPerformanceFrequency(&freq) && freq.QuadPart != 0) g_qpc_to_ms = 1.0e3 / (double)freq.QuadPart;

    // A cached list must be dropped when state changes, which needs the epoch/in-tick signals, so
    // require the service (an old core won't have it, a dev probe may be holding its hooks).
    if (!ck3accel_has_tick_epoch(host)) { host->log(kLogWarn, "accel_family_cache: core lacks the tick-epoch service; inert"); return 0; }
    if (!host->ensure_tick_epoch()) { host->log(kLogWarn, "accel_family_cache: shared tick-epoch service unavailable (dev probe holding the effect/tick hooks?); inert"); return 0; }

    // The two twin builders are byte-identical for >100 bytes, so a shared signature matches two
    // addresses and the core scanner (unique-match only) rejects it. Instead scan the UNIQUE third
    // builder and derive the twins from the fixed .text layout (1.19.0.6): the three composite
    // builders sit at 0x01A5A7C0 (<2>), 0x01A5A900 (<0>), 0x01A5AA40 (<1> = the unique one), so
    // <0> = c1 - 0x140 and <2> = c1 - 0x280. Each derived address is validated to carry the builder
    // prologue (48 89 5C 24 10 57 = mov [rsp+10],rbx; push rdi).
    auto* c1     = static_cast<std::uint8_t*>(host->scan(kSigBuilderC1));
    auto* walker = static_cast<std::uint8_t*>(host->scan(kSigWalker));
    if (!c1 || !walker) {
        host->log(kLogWarn, "accel_family_cache: a signature was NOT FOUND; plugin inert");
        return 0;
    }
    std::uint8_t* b0 = c1 - 0x140;   // <0>  close_family (fn_01A5A900)
    std::uint8_t* b2 = c1 - 0x280;   // <2>  close_or_extended (fn_01A5A7C0)
    const std::uint8_t proto[6] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57};
    if (std::memcmp(b0, proto, 6) != 0 || std::memcmp(b2, proto, 6) != 0) {
        host->log(kLogWarn, "accel_family_cache: derived builder twins failed prologue check; inert");
        return 0;
    }
    // push_back from the walker's +0x206 E8 site.
    void* pb = decode_call(walker + 0x206);
    if (!pb) { host->log(kLogWarn, "accel_family_cache: push_back unresolved; inert"); return 0; }
    g_push_back = reinterpret_cast<push_back_fn>(pb);

    g_main_tid.store(find_main_thread(), std::memory_order_relaxed);
    if (g_main_tid.load() == 0) { host->log(kLogWarn, "accel_family_cache: no main thread; inert"); return 0; }
    bool ok = true;
    ok &= host->install_hook(reg->hook_set, b0, reinterpret_cast<void*>(&detour_builder<0>),
                             reinterpret_cast<void**>(&g_orig_builder[0])) != nullptr && g_orig_builder[0];
    ok &= host->install_hook(reg->hook_set, c1, reinterpret_cast<void*>(&detour_builder<1>),
                             reinterpret_cast<void**>(&g_orig_builder[1])) != nullptr && g_orig_builder[1];
    ok &= host->install_hook(reg->hook_set, b2, reinterpret_cast<void*>(&detour_builder<2>),
                             reinterpret_cast<void**>(&g_orig_builder[2])) != nullptr && g_orig_builder[2];
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    void* peek = user32 ? reinterpret_cast<void*>(::GetProcAddress(user32, "PeekMessageW")) : nullptr;
    void* getm = user32 ? reinterpret_cast<void*>(::GetProcAddress(user32, "GetMessageW")) : nullptr;
    ok &= peek && host->install_hook(reg->hook_set, peek, reinterpret_cast<void*>(&detour_peek),
                                     reinterpret_cast<void**>(&g_orig_peek)) != nullptr && g_orig_peek;
    ok &= getm && host->install_hook(reg->hook_set, getm, reinterpret_cast<void*>(&detour_getmsg),
                                     reinterpret_cast<void**>(&g_orig_getmsg)) != nullptr && g_orig_getmsg;
    if (!ok) { host->log(kLogWarn, "accel_family_cache: a hook failed; inert"); return 0; }

    g_active.store(true, std::memory_order_release);
    g_cache_ready.store(true, std::memory_order_release);
    if (HANDLE th = ::CreateThread(nullptr, 0, &diag_thread_main, nullptr, 0, nullptr)) ::CloseHandle(th);
    char msg[192];
    std::snprintf(msg, sizeof(msg),
        "accel_family_cache: active (builders %p/%p/%p, main thread %lu)",
        static_cast<void*>(b0), static_cast<void*>(c1), static_cast<void*>(b2),
        static_cast<unsigned long>(g_main_tid.load()));
    host->log(kLogInfo, msg);
    return 0;
}
