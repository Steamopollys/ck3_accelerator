// accel_tick_cache_probe: DEVELOPER-ONLY, observe-only. NOT SHIPPED.
//
// Gating measurement for the tick trigger-result cache. Answers, with zero behavior change:
//   * If we cached trigger results keyed (node, this/prev/root, saved scopes, skip) and flushed on
//     every script effect and at each tick boundary, what fraction of the tick's trigger evals
//     would be served? (the real sound hit rate, not the naive per-day (node,entity) repeat that
//     ignores mid-tick state changes)
//   * Is it SOUND, i.e. would a cached result ever differ from a fresh one within the same epoch?
//     (mismatch count; must be ~0, else some state changes bypass the effect executor we flush on)
//
// Safety: the trigger evaluator is called EXACTLY ONCE (the game's real result), compared against
// what we cached from a PREVIOUS call in the same epoch, never a second evaluation, so no double
// side-effects and no gameplay change. Triggers that mutate the scope context
// (save_temporary_scope_value_as, temporary_list, …) are detected (saved-scope count changed
// across the call) and excluded from caching, the same rule the real cache would use.
//
// Measures only DURING a day-tick (g_in_tick), on whatever thread runs the trigger (the tick's
// parallel-for dispatches to workers), using a per-thread cache (lock-free; the shippable shape).
// Reports per day via UpdateTurnTick. SP-only; off unless tick_cache_probe.conf.

#include <ck3accel/core_api.h>
#include <ck3accel/trigger_cache.h>

#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#if defined(_WIN32)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif
static_assert(sizeof(void*) == 8, "x64 only");

namespace {
constexpr int kLogInfo = 2, kLogWarn = 3;
const CoreApi* g_host = nullptr;

const char* const kSigTrigger =
    "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 C8 FD FF FF";
const char* const kSigEffect =
    "48 8B C4 48 89 58 08 48 89 70 18 55 57 41 54 41 56 41 57 48 8D A8 88 FD FF FF 48 81 EC 50 03 00 00";
const char* const kSigTick =
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FD FF FF 48 81 EC 88 03 00 00 0F 29 B4 24 70 03 00 00";

// scope-context offsets
constexpr std::size_t kCtxThis=0x00,kCtxPrev=0x08,kCtxRoot=0x10,kCtxStore=0x18,kCtxMode=0x20;
constexpr std::size_t kStoreArr=0x00,kStoreCnt=0x0C,kStoreFb=0x3D0,kFbArr=0x18,kFbCnt=0x24;
constexpr std::size_t kPrimStride=0x20,kFbStride=0x18,kEntryRef=0x08,kMaxScopes=64;

using trigger_fn = char (*)(void* node, std::uint8_t* ctx, std::uint8_t skip);
using effect_fn  = void (*)(void* effect, void* ctx, void* a, void* b);
using tick_fn    = int  (*)(void* gamestate);
trigger_fn g_orig_trigger = nullptr;
effect_fn  g_orig_effect = nullptr;
tick_fn    g_orig_tick = nullptr;

std::atomic<bool>          g_active{false};
std::atomic<std::uint32_t> g_epoch{1};
std::atomic<int>           g_in_tick{0};
std::atomic<std::uint64_t> g_day{0};

// per-thread cache + per-thread poison set (mutating nodes)
constexpr std::size_t kCacheSlots = 1u << 18;   // 256K/thread
thread_local ck3accel::cache::EpochCache<kCacheSlots>* t_cache = nullptr;
thread_local ck3accel::cache::NodeFlagSet* t_poison = nullptr;

std::atomic<unsigned long long> g_eval{0}, g_hit{0}, g_miss{0}, g_mismatch{0}, g_mutating{0}, g_bypass{0};
unsigned long long g_prev_eval=0, g_prev_hit=0, g_prev_mismatch=0, g_prev_mut=0;

// ---- per-trigger-class (per-vtable) accumulation, for the sound-whitelist analysis ----------
std::uintptr_t g_module_base = 0;
std::uint32_t  g_image_size = 0;
constexpr std::size_t kClassSlots = 1u << 14;   // 16K vtables
struct ClassSlot {
    std::atomic<std::uint64_t> vt{0};
    std::atomic<unsigned long long> evals{0}, hits{0}, mismatch{0};
};
ClassSlot g_cls[kClassSlots];

inline void class_record(std::uint64_t vt, bool hit, bool mismatch) {
    if (!vt) return;
    std::size_t i = static_cast<std::size_t>((vt >> 4) * 0x9E3779B97F4A7C15ull >> 50) & (kClassSlots - 1);
    for (std::size_t p = 0; p < 64; ++p) {
        std::uint64_t cur = g_cls[i].vt.load(std::memory_order_relaxed);
        if (cur == vt) break;
        if (cur == 0) { std::uint64_t exp = 0;
            if (g_cls[i].vt.compare_exchange_strong(exp, vt, std::memory_order_relaxed)) break;
            if (g_cls[i].vt.load(std::memory_order_relaxed) == vt) break; }
        i = (i + 1) & (kClassSlots - 1);
    }
    g_cls[i].evals.fetch_add(1, std::memory_order_relaxed);
    if (hit) g_cls[i].hits.fetch_add(1, std::memory_order_relaxed);
    if (mismatch) g_cls[i].mismatch.fetch_add(1, std::memory_order_relaxed);
}

inline bool in_image(const void* q, std::size_t n = 8) {
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(q);
    return g_module_base && a >= g_module_base && a + n <= g_module_base + g_image_size;
}
// MSVC RTTI: vtable[-1] -> CompleteObjectLocator; [COL+0xC] = TypeDescriptor RVA; td+0x10 = name.
bool class_name(std::uint64_t vt, char* out, std::size_t cap) {
    __try {
        const std::uint8_t* v = reinterpret_cast<const std::uint8_t*>(vt);
        if (!in_image(v - 8)) return false;
        const std::uint8_t* col = *reinterpret_cast<const std::uint8_t* const*>(v - 8);
        if (!in_image(col, 24)) return false;
        std::uint32_t td_rva; std::memcpy(&td_rva, col + 0xC, 4);
        const std::uint8_t* td = reinterpret_cast<const std::uint8_t*>(g_module_base) + td_rva;
        if (!in_image(td, 0x20)) return false;
        const char* m = reinterpret_cast<const char*>(td + 0x10);
        if (m[0] != '.' || m[1] != '?') return false;
        std::size_t o = 0;
        for (const char* c = m + 4; *c && o + 1 < cap && c < m + 300; ++c) {
            if (c[0] == '@' && c[1] == '@') { ++c; continue; }
            out[o++] = (*c == '@') ? ':' : *c;
        }
        out[o] = 0; return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

inline void bump() { g_epoch.fetch_add(1, std::memory_order_relaxed); }

bool read_context(std::uint8_t* ctx, void* node, std::uint8_t skip, ck3accel::cache::ContextView* out,
                  ck3accel::cache::SavedScope* prim, ck3accel::cache::SavedScope* fb) {
    __try {
        if (ctx[kCtxMode] == 2) return false;
        out->node = reinterpret_cast<std::uint64_t>(node);
        out->current = reinterpret_cast<ck3accel::cache::ScopeRef*>(*reinterpret_cast<void**>(ctx + kCtxThis));
        out->prev    = reinterpret_cast<ck3accel::cache::ScopeRef*>(*reinterpret_cast<void**>(ctx + kCtxPrev));
        out->root    = reinterpret_cast<ck3accel::cache::ScopeRef*>(*reinterpret_cast<void**>(ctx + kCtxRoot));
        out->skip_validation = skip;
        out->saved = prim; out->saved_count = 0; out->fallback = fb; out->fallback_count = 0;
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
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool read_counts(std::uint8_t* ctx, std::uint32_t* p, std::uint32_t* f) {
    __try {
        std::uint8_t* store = *reinterpret_cast<std::uint8_t**>(ctx + kCtxStore);
        if (!store) { *p = 0; *f = 0; return true; }
        *p = *reinterpret_cast<std::uint32_t*>(store + kStoreCnt);
        std::uint8_t* fb = *reinterpret_cast<std::uint8_t**>(store + kStoreFb);
        *f = fb ? *reinterpret_cast<std::uint32_t*>(fb + kFbCnt) : 0u;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::uint64_t read_vtable(void* node) {
    __try { return *reinterpret_cast<std::uint64_t*>(node); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

char detour_trigger(void* node, std::uint8_t* ctx, std::uint8_t skip) {
    // Only measure during a tick; outside it, pass straight through (this probe is about tick cost).
    if (!g_active.load(std::memory_order_relaxed) || g_in_tick.load(std::memory_order_relaxed) == 0 ||
        !node || !ctx) {
        return g_orig_trigger(node, ctx, skip);
    }
    if (!t_cache) { t_cache = new ck3accel::cache::EpochCache<kCacheSlots>(); t_poison = new ck3accel::cache::NodeFlagSet(); }
    g_eval.fetch_add(1, std::memory_order_relaxed);

    if (t_poison->contains(reinterpret_cast<std::uint64_t>(node))) {
        g_mutating.fetch_add(1, std::memory_order_relaxed);
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

    // Call the real trigger EXACTLY ONCE (the result the game uses), watching for a mutation.
    std::uint32_t p0=0,f0=0; const bool h0 = read_counts(ctx,&p0,&f0);
    const char rc = g_orig_trigger(node, ctx, skip);
    std::uint32_t p1=0,f1=0; const bool h1 = read_counts(ctx,&p1,&f1);
    if (h0 && h1 && (p0 != p1 || f0 != f1)) {   // mutating trigger: exclude from caching
        t_poison->insert(reinterpret_cast<std::uint64_t>(node));
        g_mutating.fetch_add(1, std::memory_order_relaxed);
        return rc;
    }
    // Compare against what we cached from a prior call in this epoch (observe-only).
    std::uint8_t cached = 0; bool hit = false, mm = false;
    if (t_cache->lookup(key, epoch, &cached)) {
        hit = true; g_hit.fetch_add(1, std::memory_order_relaxed);
        if (static_cast<char>(cached) != rc) { mm = true; g_mismatch.fetch_add(1, std::memory_order_relaxed); }
    } else {
        g_miss.fetch_add(1, std::memory_order_relaxed);
    }
    class_record(read_vtable(node), hit, mm);
    t_cache->store(key, epoch, static_cast<std::uint8_t>(rc));
    return rc;   // ALWAYS the fresh result
}

void detour_effect(void* e, void* c, void* a, void* b) { bump(); g_orig_effect(e, c, a, b); bump(); }

FILE* g_csv = nullptr;
std::wstring install_dir() {
    HMODULE self=nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&install_dir), &self);
    wchar_t buf[MAX_PATH]; DWORD n=::GetModuleFileNameW(self,buf,MAX_PATH); if(!n||n>=MAX_PATH) return L"";
    std::wstring p(buf,n); auto l=p.find_last_of(L"\\/"); if(l==std::wstring::npos) return L"";
    std::wstring pl=p.substr(0,l); auto pv=pl.find_last_of(L"\\/"); return pv==std::wstring::npos?L"":pl.substr(0,pv);
}

void dump_classes() {
    const std::wstring dir = install_dir(); if (dir.empty()) return;
    const std::wstring path = dir + L"\\logs\\tick_cache_classes.txt";
    FILE* f = _wfsopen(path.c_str(), L"w", 0x40); if (!f) return;
    // collect
    struct Row { std::uint64_t vt; unsigned long long ev, hi, mm; };
    std::vector<Row> rows;
    for (std::size_t i = 0; i < kClassSlots; ++i) {
        std::uint64_t vt = g_cls[i].vt.load(std::memory_order_relaxed);
        if (!vt) continue;
        rows.push_back({vt, g_cls[i].evals.load(), g_cls[i].hits.load(), g_cls[i].mismatch.load()});
    }
    std::fprintf(f, "# trigger classes by cacheability (evals, hits, mismatches). vt=vtable rva\n");
    std::fprintf(f, "## IMPURE (any mismatch) — exclude these from a sound cache:\n");
    std::sort(rows.begin(), rows.end(), [](const Row&a,const Row&b){return a.mm>b.mm;});
    for (const auto& r : rows) { if (r.mm == 0) break;
        char nm[256]="?"; class_name(r.vt, nm, sizeof(nm));
        std::fprintf(f, "  mm=%llu ev=%llu hit=%llu  vt=%08llX  %s\n", r.mm, r.ev, r.hi,
                     (unsigned long long)(r.vt - g_module_base), nm); }
    std::fprintf(f, "## PURE, high-volume (zero mismatch) — the sound whitelist, sorted by hits saved:\n");
    std::sort(rows.begin(), rows.end(), [](const Row&a,const Row&b){return a.hi>b.hi;});
    unsigned long long pure_hits=0, pure_ev=0;
    for (const auto& r : rows) { if (r.mm != 0) continue; pure_hits += r.hi; pure_ev += r.ev; }
    int shown=0;
    for (const auto& r : rows) { if (r.mm != 0 || r.hi == 0) continue; if (shown++ >= 40) break;
        char nm[256]="?"; class_name(r.vt, nm, sizeof(nm));
        std::fprintf(f, "  hit=%llu ev=%llu  vt=%08llX  %s\n", r.hi, r.ev,
                     (unsigned long long)(r.vt - g_module_base), nm); }
    std::fprintf(f, "# pure-class totals: hits=%llu evals=%llu\n", pure_hits, pure_ev);
    std::fclose(f);
    if (g_host && g_host->log) { char msg[160];
        std::snprintf(msg, sizeof(msg), "accel_tick_cache_probe: wrote class breakdown (pure-class hits=%llu) -> tick_cache_classes.txt", pure_hits);
        g_host->log(kLogWarn, msg); }
}

int detour_tick(void* gs) {
    g_in_tick.fetch_add(1, std::memory_order_relaxed);
    bump();
    const int rc = g_orig_tick(gs);
    bump();
    g_in_tick.fetch_sub(1, std::memory_order_relaxed);
    const std::uint64_t day = g_day.fetch_add(1, std::memory_order_relaxed);
    if (day > 0 && day % 32 == 0) dump_classes();

    const unsigned long long ev=g_eval, hi=g_hit, mm=g_mismatch, mu=g_mutating;
    const unsigned long long d_ev=ev-g_prev_eval, d_hi=hi-g_prev_hit, d_mm=mm-g_prev_mismatch, d_mu=mu-g_prev_mut;
    g_prev_eval=ev; g_prev_hit=hi; g_prev_mismatch=mm; g_prev_mut=mu;
    if (d_ev > 0) {
        const double hit_pct = 100.0*(double)d_hi/(double)d_ev;
        const double mm_pct  = d_hi ? 100.0*(double)d_mm/(double)d_hi : 0.0;
        if (g_host && g_host->log && (day % 8 == 0 || d_ev > 5000000)) {
            char msg[224];
            std::snprintf(msg,sizeof(msg),
              "accel_tick_cache_probe: day %llu evals=%llu hit=%.1f%% mismatch=%llu (%.4f%% of hits) mutating=%llu",
              (unsigned long long)day, d_ev, hit_pct, d_mm, mm_pct, d_mu);
            g_host->log(kLogWarn, msg);
        }
        if (g_csv) { std::fprintf(g_csv,"%llu,%llu,%llu,%llu,%llu\n",
              (unsigned long long)day,d_ev,d_hi,d_mm,d_mu); std::fflush(g_csv); }
    }
    return rc;
}

bool conf_enabled() {
    const std::wstring dir = install_dir(); if (dir.empty()) return false;
    FILE* f=nullptr; if (_wfopen_s(&f,(dir+L"\\tick_cache_probe.conf").c_str(),L"rb")!=0||!f) return false;
    char t[256]; size_t n=std::fread(t,1,sizeof(t)-1,f); t[n]=0; std::fclose(f);
    std::string s(t); return s.find("tick_cache_probe=true")!=std::string::npos || s.find("tick_cache_probe = true")!=std::string::npos;
}

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)), CK3ACCEL_PLUGIN_MAGIC, CK3ACCEL_ABI_VERSION,
    "accel_tick_cache_probe","0.1.0","1.19.0.6","1.19.0.6", CK3ACCEL_MODE_SP,
};
}  // namespace

PLUGIN_EXPORT const CK3AccelPluginInfo* CK3Accel_Query(uint32_t v){ (void)v; return &kInfo; }

PLUGIN_EXPORT int CK3Accel_Init(const CoreApi* host, CK3AccelRegistrar* reg) {
    g_host = host;
    if (!host || !host->log || !host->scan || !host->install_hook || !reg) return 1;
    if (!conf_enabled()) { host->log(kLogInfo,"accel_tick_cache_probe: disabled (tick_cache_probe.conf); inert"); return 0; }
    g_module_base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    { MODULEINFO mi{}; if (::GetModuleInformation(::GetCurrentProcess(), ::GetModuleHandleW(nullptr), &mi, sizeof(mi))) g_image_size = mi.SizeOfImage; }
    void* trig = host->scan(kSigTrigger);
    void* eff  = host->scan(kSigEffect);
    void* tick = host->scan(kSigTick);
    if (!trig || !eff || !tick) { host->log(kLogWarn,"accel_tick_cache_probe: signature NOT FOUND; inert"); return 0; }
    const std::wstring dir = install_dir();
    if (!dir.empty()) {
        const std::wstring logs = dir + L"\\logs"; ::CreateDirectoryW(logs.c_str(), nullptr);
        const std::wstring path = logs + L"\\tick_cache_probe.csv";
        const bool existed = ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        g_csv = _wfsopen(path.c_str(), L"a", 0x40 /*_SH_DENYNO*/);
        if (g_csv && !existed) { std::fputs("day,evals,hits,mismatches,mutating_dummy,mutating\n", g_csv); std::fflush(g_csv); }
    }
    bool ok = true;
    ok &= host->install_hook(reg->hook_set, tick, reinterpret_cast<void*>(&detour_tick), reinterpret_cast<void**>(&g_orig_tick))!=nullptr && g_orig_tick;
    ok &= host->install_hook(reg->hook_set, eff,  reinterpret_cast<void*>(&detour_effect), reinterpret_cast<void**>(&g_orig_effect))!=nullptr && g_orig_effect;
    ok &= host->install_hook(reg->hook_set, trig, reinterpret_cast<void*>(&detour_trigger), reinterpret_cast<void**>(&g_orig_trigger))!=nullptr && g_orig_trigger;
    if (!ok) { host->log(kLogWarn,"accel_tick_cache_probe: a hook failed; inert"); return 0; }
    g_active.store(true, std::memory_order_release);
    host->log(kLogInfo,"accel_tick_cache_probe: active (observe-only; measures in-tick trigger cacheability + soundness)");
    return 0;
}
