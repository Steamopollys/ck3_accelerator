// accel_tick_cache: shipping plugin, opt-in.
//
// Memoizes pure leaf-trigger results during the day-tick, keyed on (node, this/prev/root, saved
// scopes, skip). Only trigger classes a soundness probe proved pure functions of their scope get
// cached (has_trait, is_ruler, is_landed, government flags, family relations, doctrines, …).
// Combinators (And/Or/Not/If), scripted triggers, and the few impure leaves (AI-agent validity,
// has_perk, has_realm_law) always re-evaluate; that's cheap, since their expensive pure children
// hit the cache. In practice ~45% of in-tick trigger evals are served, with zero divergence.
//
// Soundness: the class is resolved once per vtable via RTTI and remembered; a hit counts only
// within its epoch; the shared tick-epoch service bumps the epoch on every effect and tick
// boundary, so nothing stale survives a state change. If a whitelisted trigger mutates its scope
// (saved-scope count moves across the call), we catch it and drop that node for good. Result:
// byte-identical to a fresh eval, checksum-safe for SP + Ironman.
//
// The epoch/in-tick signals come from the core's tick-epoch service, so this coexists with
// accel_family_cache rather than fighting it for the effect/tick hooks. Off unless tick_cache.conf
// enables it.

#include <ck3accel/core_api.h>
#include <ck3accel/trigger_cache.h>
#include <ck3accel/rtti.h>

#include <windows.h>
#include <psapi.h>

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
static_assert(sizeof(void*) == 8, "x64 only");

namespace {
constexpr int kLogInfo = 2, kLogWarn = 3;
const CoreApi* g_host = nullptr;
std::uintptr_t g_module_base = 0;
std::uint32_t  g_image_size = 0;

// (the core owns the effect/tick hooks via the tick-epoch service, and the trigger evaluator via the
// trigger service; this plugin registers a handler rather than hooking anything itself.)

// The cacheable classes, matched by demangled-name prefix. Each one had 0 mismatch over the 184-day
// probe and is a leaf predicate reading a stable scope property. Combinators, wrappers, scripted
// triggers, and the impure leaves are left out on purpose.
const char* const kWhitelist[] = {
    // CJominiContextTrigger and CJominiEventTargetComparisonTrigger measured result-pure but push a
    // saved scope as a side effect (the poison detector flagged both), so they're out: caching a
    // node that skips that push would be a data-dependent trap. CJominiTargetExistsTrigger just
    // tests existence, no push, so it stays.
    "CJominiTargetExistsTrigger",
    "CJominiCompareValueOfScopeObjectTrigger",
    "CHasTraitTrigger",
    "CHasScriptedRelationTrigger",
    "CHasCharacterFlagTrigger",
    "CHasOpinionModifierTrigger",
    "CHasModifierTrigger",
    "CHasHouseRelationLevel",
    "CHasDreadlevelTowardsTrigger",
    "CHasDoctrineTrigger",
    "CHasDoctrineParameterTrigger",
    "CHasCultureParameterTrigger",
    "CHasGraphicalCultureTrigger",
    "CHasCulturalEraOrLater",
    "CGovernmentFlagTrigger",
    "CGovernmentRuleTrigger",
    "CGameRuleSettingTrigger",
    "CScopedBoolConditionTrigger:VCIsAliveCondition",
    "CScopedBoolConditionTrigger:VCIsRulerCondition",
    "CIsLandedTrigger",
    "CIsAdult",
    "CIsAITrigger",
    "CIsIncapableTrigger",
    "CIsChildOfTrigger",
    "CIsSpouseOfTrigger",
    "CIsGuestOfTrigger",
    "CHasCourtPositionTrigger",
    "CHasCouncilPositionTrigger",
    "CTargetIsLiegeOrAboveTrigger",
    "CSchemeSkillTrigger",
    "CIsSchemeTargetType",
    "CHasSchemeCountermeasureParameter",
    "CPlaceInLineOfSuccessionTrigger",
    "CHasDomicileBuildingParameter",
};

// scope-context offsets
constexpr std::size_t kCtxThis=0x00,kCtxPrev=0x08,kCtxRoot=0x10,kCtxStore=0x18,kCtxMode=0x20;
constexpr std::size_t kStoreArr=0x00,kStoreCnt=0x0C,kStoreFb=0x3D0,kFbArr=0x18,kFbCnt=0x24;
constexpr std::size_t kPrimStride=0x20,kFbStride=0x18,kEntryRef=0x08,kMaxScopes=64;

// trigger eval arrives via the core's shared trigger service (coexists with the override demo);
// next() in tick_handler continues the chain to the original evaluator.

std::atomic<bool>          g_active{false};
// Epoch + in-tick come from the core's shared tick-epoch service (g_host->tick_epoch / in_tick).

constexpr std::size_t kCacheSlots = 1u << 18;   // per-thread result cache (256K)
thread_local ck3accel::cache::EpochCache<kCacheSlots>* t_cache = nullptr;

// per-vtable decision cache: 0 unknown, 1 pure(cacheable), 2 impure(bypass). Shared, atomic slots.
constexpr std::size_t kDecSlots = 1u << 14;
struct Dec { std::atomic<std::uint64_t> vt{0}; std::atomic<std::uint8_t> d{0}; };
Dec g_dec[kDecSlots];

int g_enabled = 1;   // runtime on/off, flipped from the overlay panel
std::atomic<unsigned long long> g_hit{0}, g_miss{0}, g_poison{0};
unsigned long long g_prev_hit=0, g_prev_miss=0;

// Home slot for a vtable in the open-addressed decision table (shared by decision_for/mark_impure).
inline std::size_t dec_home(std::uint64_t vt) {
    return static_cast<std::size_t>((vt >> 4) * 0x9E3779B97F4A7C15ull >> 50) & (kDecSlots - 1);
}

bool name_whitelisted(const char* nm) {
    for (const char* w : kWhitelist) {
        const std::size_t n = std::strlen(w);
        if (std::strncmp(nm, w, n) == 0) return true;   // prefix match
    }
    return false;
}
// Decide (and remember) whether a vtable is cacheable.
std::uint8_t decision_for(std::uint64_t vt) {
    if (!vt) return 2;
    std::size_t i = dec_home(vt);
    for (std::size_t p = 0; p < 64; ++p) {
        std::uint64_t cur = g_dec[i].vt.load(std::memory_order_relaxed);
        if (cur == vt) return g_dec[i].d.load(std::memory_order_relaxed);
        if (cur == 0) {
            char nm[256] = {0};
            const std::uint8_t d = (ck3accel::rtti::class_name({g_module_base, g_image_size}, vt, nm, sizeof(nm)) && name_whitelisted(nm)) ? 1 : 2;
            std::uint64_t exp = 0;
            if (g_dec[i].vt.compare_exchange_strong(exp, vt, std::memory_order_relaxed)) {
                g_dec[i].d.store(d, std::memory_order_relaxed); return d;
            }
            if (g_dec[i].vt.load(std::memory_order_relaxed) == vt) return g_dec[i].d.load(std::memory_order_relaxed);
        }
        i = (i + 1) & (kDecSlots - 1);
    }
    return 2;   // table full: don't cache
}
void mark_impure(std::uint64_t vt) {   // a whitelisted node that mutated scope → never cache it
    std::size_t i = dec_home(vt);
    for (std::size_t p = 0; p < 64; ++p) {
        if (g_dec[i].vt.load(std::memory_order_relaxed) == vt) {
            // Log the class name once, on the 1→2 transition, so the whitelist can be corrected.
            if (g_dec[i].d.exchange(2, std::memory_order_relaxed) != 2 && g_host && g_host->log) {
                char nm[256] = "?"; ck3accel::rtti::class_name({g_module_base, g_image_size}, vt, nm, sizeof(nm));
                char msg[320];
                std::snprintf(msg, sizeof(msg),
                    "accel_tick_cache: POISONED whitelisted class '%s' (mutates scope) — excluded; drop from whitelist", nm);
                g_host->log(kLogWarn, msg);
            }
            return;
        }
        i = (i + 1) & (kDecSlots - 1);
    }
}

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

// Registered with the core's shared trigger service. next() continues the chain (downstream handlers,
// then the original evaluator).
char tick_handler(void* node, void* ctxv, unsigned char skip, ck3accel_trigger_next next, void* next_ctx, void*) {
    std::uint8_t* ctx = static_cast<std::uint8_t*>(ctxv);
    if (!g_enabled || !g_active.load(std::memory_order_relaxed) || !node || !ctx || g_host->in_tick() == 0) {
        return next(node, ctxv, skip, next_ctx);
    }
    const std::uint64_t vt = ck3accel::rtti::read_vtable(node);
    if (decision_for(vt) != 1) return next(node, ctxv, skip, next_ctx);   // not a cacheable pure class

    if (!t_cache) t_cache = new ck3accel::cache::EpochCache<kCacheSlots>();
    ck3accel::cache::ContextView v{};
    ck3accel::cache::SavedScope prim[kMaxScopes], fb[kMaxScopes];
    if (!read_context(ctx, node, skip, &v, prim, fb)) return next(node, ctxv, skip, next_ctx);
    const ck3accel::cache::ContextKey key = ck3accel::cache::make_key(v);
    const std::uint32_t epoch = g_host->tick_epoch();

    std::uint8_t cached = 0;
    if (t_cache->lookup(key, epoch, &cached)) {          // HIT: serve the cached result, skip the chain
        g_hit.fetch_add(1, std::memory_order_relaxed);
        return static_cast<char>(cached);
    }
    // Miss: run the chain once, watching for a whitelisted node that unexpectedly mutates scope. Node
    // eval is single-threaded, so read_context's counts are still the pre-call snapshot; reuse them.
    const std::uint32_t p0 = static_cast<std::uint32_t>(v.saved_count);
    const std::uint32_t f0 = static_cast<std::uint32_t>(v.fallback_count);
    const char rc = next(node, ctxv, skip, next_ctx);
    std::uint32_t p1=0,f1=0; const bool h1 = read_counts(ctx,&p1,&f1);
    if (h1 && (p0 != p1 || f0 != f1)) { mark_impure(vt); g_poison.fetch_add(1, std::memory_order_relaxed); return rc; }
    t_cache->store(key, epoch, static_cast<std::uint8_t>(rc));
    g_miss.fetch_add(1, std::memory_order_relaxed);
    return rc;
}

// With no tick hook of our own to log from, a slow background thread reports the hit rate.
DWORD WINAPI diag_thread_main(LPVOID) {
    for (;;) {
        ::Sleep(8000);
        if (!g_active.load(std::memory_order_relaxed) || !g_host || !g_host->log) continue;
        const unsigned long long hi = g_hit.load(), mi = g_miss.load();
        const unsigned long long d_hi = hi - g_prev_hit, d_mi = mi - g_prev_miss;
        g_prev_hit = hi; g_prev_miss = mi;
        const unsigned long long served = d_hi + d_mi;
        if (served == 0) continue;   // paused / not in a tick: nothing to report
        char msg[192];
        std::snprintf(msg, sizeof(msg),
            "accel_tick_cache: whitelisted-evals=%llu hits=%.1f%% (poisoned nodes=%llu)",
            served, 100.0 * (double)d_hi / (double)served, (unsigned long long)g_poison.load());
        g_host->log(kLogInfo, msg);
    }
}

bool conf_enabled() {
    HMODULE self=nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&conf_enabled), &self);
    wchar_t buf[MAX_PATH]; DWORD n=::GetModuleFileNameW(self,buf,MAX_PATH); if(!n||n>=MAX_PATH) return false;
    std::wstring p(buf,n); auto l=p.find_last_of(L"\\/"); if(l==std::wstring::npos) return false;
    std::wstring pl=p.substr(0,l); auto pv=pl.find_last_of(L"\\/"); if(pv==std::wstring::npos) return false;
    FILE* f=nullptr; if(_wfopen_s(&f,(pl.substr(0,pv)+L"\\tick_cache.conf").c_str(),L"rb")!=0||!f) return false;
    char t[256]; size_t r=std::fread(t,1,sizeof(t)-1,f); t[r]=0; std::fclose(f);
    std::string s(t); return s.find("tick_cache=true")!=std::string::npos||s.find("tick_cache = true")!=std::string::npos;
}

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)), CK3ACCEL_PLUGIN_MAGIC, CK3ACCEL_ABI_VERSION,
    "accel_tick_cache","0.1.0","1.19.0.6","1.19.0.6", CK3ACCEL_MODE_SP | CK3ACCEL_MODE_IRONMAN,
};
}  // namespace

PLUGIN_EXPORT const CK3AccelPluginInfo* CK3Accel_Query(uint32_t v){ (void)v; return &kInfo; }

PLUGIN_EXPORT int CK3Accel_Init(const CoreApi* host, CK3AccelRegistrar* reg) {
    g_host = host;
    if (!host || !host->log || !reg) return 1;
    if (!conf_enabled()) { host->log(kLogInfo,"accel_tick_cache: disabled (set tick_cache=true in tick_cache.conf); inert"); return 0; }

    // Needs the epoch/in-tick signals AND the shared trigger hook; bail if either is missing.
    if (!ck3accel_has_tick_epoch(host) || !host->ensure_tick_epoch()) { host->log(kLogWarn,"accel_tick_cache: tick-epoch service unavailable; inert"); return 0; }
    if (!ck3accel_has_trigger_service(host) || !host->ensure_trigger_service()) { host->log(kLogWarn,"accel_tick_cache: trigger service unavailable; inert"); return 0; }

    g_module_base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    { MODULEINFO mi{}; if (::GetModuleInformation(::GetCurrentProcess(), ::GetModuleHandleW(nullptr), &mi, sizeof(mi))) g_image_size = mi.SizeOfImage; }
    host->register_trigger_handler(&tick_handler, nullptr, /*priority=*/0);   // inner (after any override)
    g_active.store(true, std::memory_order_release);
    if (HANDLE t = ::CreateThread(nullptr, 0, &diag_thread_main, nullptr, 0, nullptr)) ::CloseHandle(t);
    if (ck3accel_has_panels(host)) {
        CK3AccelPanel panel{};
        panel.struct_size = sizeof(panel);
        panel.name = "tick cache (in-tick triggers)";
        panel.enabled = &g_enabled;
        panel.stat_count = 2;
        panel.stat_labels[0] = "hits";   panel.stat_values[0] = reinterpret_cast<const unsigned long long*>(&g_hit);
        panel.stat_labels[1] = "misses"; panel.stat_values[1] = reinterpret_cast<const unsigned long long*>(&g_miss);
        host->register_panel(&panel);
    }
    host->log(kLogInfo,"accel_tick_cache: active (whitelisted pure-trigger cache, in-tick, checksum-neutral)");
    return 0;
}
