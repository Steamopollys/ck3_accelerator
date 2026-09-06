// accel_family_lists: shipping plugin.
//
// Replaces ck3.exe's O(N^2) dedup of the composite family scope lists
// (`close_family_member`, `extended_family_member`, `close_or_extended_family_member`,
// any_/every_/random_/ordered_ variants) with an O(N) hash-set dedup. The engine builds them
// through two primitives that linearly scan the whole output list per relative added: the
// add-unique helper and the family walker (which inlines a second copy of the scan). Both are
// hooked and reimplemented on <ck3accel/family_dedup.h>, whose output is proven entry-for-entry
// identical to the engine's by core/test/test_family_dedup.cpp.
//
// State: a thread-local mirror set keyed on the list OBJECT (address, buffer pointer, element
// count, last entry). Every push a composite build makes goes through one of the two hooked
// functions (verified by disassembly of all six builder helpers), so the mirror stays exact;
// on any mismatch it is rebuilt from the list's real contents (O(count), normally at count 0).
// Falls back to the original if any signature or the decoded globals fail to resolve.
//
// SP + Ironman (results identical, so checksum-neutral; multiplayer declined by project policy).

#include <ck3accel/core_api.h>
#include <ck3accel/family_dedup.h>

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif

static_assert(sizeof(void*) == 8, "accel_family_lists is x64-only");

namespace {
constexpr int kLogInfo = 2;
constexpr int kLogWarn = 3;

const CoreApi* g_host = nullptr;

// ---- signatures (facts about a public binary; no ck3.exe bytes are shipped) --------------
// Family walker fn_01A5F8C0: walk(ctx, character, skipctx) adds a character's children +
// grandchildren into ctx's list, skipping [skipctx+0x18].
const char* const kSigWalker =
    "4C 89 44 24 18 41 54 41 55 48 83 EC 58 48 8B 82 A0 01 00 00 4C 8B E9 48 85 C0 74 ??";
// Add-unique fn_02688C70: add_unique(list, character).
const char* const kSigAddUnique =
    "48 83 EC 38 48 8B 01 45 33 D2 4C 63 41 0C 44 8B 4A 18 49 C1 E0 04 4C 03 C0 "
    "C7 44 24 20 04 00 00 00";

// ---- engine layouts (1.19.0.6, both builds) ------------------------------------------------
constexpr std::size_t kCharHandle    = 0x18;   // uint32 handle
constexpr std::size_t kCharValidSub  = 0x10;   // sub-object whose vtable slot 1 is IsValid()
constexpr std::size_t kCharFamily    = 0x1a0;  // family block pointer
constexpr std::size_t kCharDeath     = 0x1c8;  // death data pointer (null = alive)
constexpr std::size_t kFamChildren   = 0x38;   // CPdxArray<uint32> inside the family block
constexpr std::size_t kDbTable       = 0x20;   // character db: entry table (16-byte entries)
constexpr std::size_t kDbCount       = 0x2c;   // character db: entry count
constexpr std::size_t kFilterAll     = 0x08;   // filter flags: include everyone
constexpr std::size_t kFilterDead    = 0x09;   // filter flags: dead only (else alive only)

struct PdxU32Array { std::uint32_t* data; std::uint32_t cap; std::int32_t count; };
struct ScopeList   { ck3accel::family::ScopeRef* data; std::uint32_t cap; std::int32_t count; void* alloc; };

using walker_fn     = void (*)(void* ctx, std::uint8_t* character, std::uint8_t* skipctx);
using add_unique_fn = void (*)(ScopeList* list, std::uint8_t* character);
using push_back_fn  = void* (*)(ScopeList* list, const ck3accel::family::ScopeRef* entry);
using is_valid_fn   = bool (*)(void* sub);

walker_fn     g_orig_walker     = nullptr;
add_unique_fn g_orig_add_unique = nullptr;
push_back_fn  g_push_back       = nullptr;
std::uint8_t** g_db_global      = nullptr;   // address of the character-db pointer global
std::uint8_t** g_null_global    = nullptr;   // address of the null-character sentinel pointer
PdxU32Array*   g_empty_array    = nullptr;   // shared empty CPdxArray used for a null family block
std::atomic<bool> g_active{false};
int g_enabled = 1;   // runtime on/off, flipped from the overlay panel

// ---- stats (info log every 60 s while they change) -----------------------------------------
std::atomic<unsigned long long> g_builds{0};          // mirror resyncs (≈ list builds)
std::atomic<unsigned long long> g_pushes{0};          // entries appended through us
std::atomic<unsigned long long> g_saved_compares{0};  // Σ list size at each membership test
unsigned long long g_last_logged_pushes = 0;

// ---- engine adapter for family_dedup.h -----------------------------------------------------
struct GameEngine {
    using Char = std::uint8_t;
    using List = ScopeList;

    Char* resolve(std::uint32_t h) const {
        std::uint8_t* db = *g_db_global;
        std::uint8_t* null_char = *g_null_global;
        if (!db) return null_char;
        const std::uint32_t idx = h & 0xFFFFFFu;
        if (idx >= *reinterpret_cast<const std::uint32_t*>(db + kDbCount)) return null_char;
        std::uint8_t* table = *reinterpret_cast<std::uint8_t**>(db + kDbTable);
        std::uint8_t* obj = *reinterpret_cast<std::uint8_t**>(table + static_cast<std::size_t>(idx) * 16 + 8);
        if (!obj) return null_char;
        if (*reinterpret_cast<const std::uint32_t*>(obj + kCharHandle) != h) return null_char;
        return obj;
    }
    bool is_valid(Char* c) const {
        void* sub = c + kCharValidSub;
        void** vt = *reinterpret_cast<void***>(sub);
        return reinterpret_cast<is_valid_fn>(vt[1])(sub);
    }
    bool is_dead(Char* c) const { return *reinterpret_cast<void**>(c + kCharDeath) != nullptr; }
    std::uint32_t handle_of(Char* c) const { return *reinterpret_cast<const std::uint32_t*>(c + kCharHandle); }
    const PdxU32Array* children(Char* c) const {
        std::uint8_t* fam = *reinterpret_cast<std::uint8_t**>(c + kCharFamily);
        return fam ? reinterpret_cast<const PdxU32Array*>(fam + kFamChildren) : g_empty_array;
    }
    const std::uint32_t* children_begin(Char* c) const { return children(c)->data; }
    const std::uint32_t* children_end(Char* c) const {
        const PdxU32Array* a = children(c);
        return a->data + (a->count > 0 ? a->count : 0);
    }
    const ck3accel::family::ScopeRef* list_data(List* l) const { return l->data; }
    std::int32_t list_count(List* l) const { return l->count; }
    void push_back(List* l, const ck3accel::family::ScopeRef& e) const {
        g_push_back(l, &e);
        g_pushes.fetch_add(1, std::memory_order_relaxed);
    }
};

// ---- thread-local mirror of "which character handles are in this list" --------------------
struct Mirror {
    ScopeList* list = nullptr;
    void* buffer = nullptr;
    std::int32_t count = -1;
    std::uint64_t last_handle = 0;
    ck3accel::family::HandleSet set;
};
thread_local Mirror* t_mirror = nullptr;

Mirror& mirror() {
    if (!t_mirror) t_mirror = new Mirror();   // once per thread; lives to process exit
    return *t_mirror;
}

// Bring the mirror in sync with `list` (cheap no-op when nothing changed since our last push).
void sync(GameEngine& e, Mirror& m, ScopeList* list) {
    const std::int32_t n = list->count;
    if (m.list == list && m.buffer == list->data && m.count == n &&
        (n <= 0 || list->data[n - 1].handle == m.last_handle)) {
        return;
    }
    ck3accel::family::sync_set_from_list(e, m.set, list);
    m.list = list;
    m.buffer = list->data;
    m.count = n;
    m.last_handle = (n > 0) ? list->data[n - 1].handle : 0;
    g_builds.fetch_add(1, std::memory_order_relaxed);
}

void note_pushed(Mirror& m, ScopeList* list) {
    m.buffer = list->data;
    m.count = list->count;
    m.last_handle = (list->count > 0) ? list->data[list->count - 1].handle : 0;
}

// ---- detours ---------------------------------------------------------------------------------
void detour_add_unique(ScopeList* list, std::uint8_t* character) {
    if (!g_enabled || !g_active.load(std::memory_order_relaxed) || !list || !character || !*g_null_global) {
        g_orig_add_unique(list, character);   // !g_enabled = runtime toggle (overlay panel)
        return;
    }
    GameEngine e;
    Mirror& m = mirror();
    sync(e, m, list);
    g_saved_compares.fetch_add(static_cast<unsigned long long>(list->count > 0 ? list->count : 0),
                               std::memory_order_relaxed);
    ck3accel::family::add_unique(e, m.set, list, character);
    note_pushed(m, list);
}

void detour_walker(void* ctx, std::uint8_t* character, std::uint8_t* skipctx) {
    if (!g_enabled || !g_active.load(std::memory_order_relaxed) || !ctx || !character || !*g_null_global) {
        g_orig_walker(ctx, character, skipctx);   // !g_enabled = runtime toggle (overlay panel)
        return;
    }
    // ctx -> pair { filter*, list* }
    void** pair = *reinterpret_cast<void***>(ctx);
    std::uint8_t* filter = static_cast<std::uint8_t*>(pair[0]);
    ScopeList* list = static_cast<ScopeList*>(pair[1]);
    if (!filter || !list) {
        g_orig_walker(ctx, character, skipctx);
        return;
    }
    GameEngine e;
    Mirror& m = mirror();
    sync(e, m, list);
    const ck3accel::family::Filter f{filter[kFilterAll] != 0, filter[kFilterDead] != 0};
    const std::uint32_t skip = skipctx ? e.handle_of(skipctx) : ck3accel::family::kInvalidHandle;
    // Σ list sizes the original would have scanned ≈ one scan per candidate; count candidates
    // cheaply as (children + grandchildren visited) × current size at entry.
    const PdxU32Array* kids = e.children(character);
    g_saved_compares.fetch_add(static_cast<unsigned long long>(kids->count > 0 ? kids->count : 0) *
                               static_cast<unsigned long long>(list->count > 0 ? list->count : 0),
                               std::memory_order_relaxed);
    ck3accel::family::walk(e, m.set, list, f, character, skip);
    note_pushed(m, list);
}

// ---- resolution helpers ----------------------------------------------------------------------
// Decode the rel32 of an E8 call at `site`; null if the byte there is not E8.
void* decode_call(const std::uint8_t* site) {
    if (site[0] != 0xE8) return nullptr;
    std::int32_t rel; std::memcpy(&rel, site + 1, 4);
    return const_cast<std::uint8_t*>(site + 5 + rel);
}

// Find the first `prefix` (e.g. 4C 8B 05 = mov r8,[rip+disp32]) inside [fn, fn+span) and
// return the RIP-relative target address.
void* find_rip_ref(const std::uint8_t* fn, std::size_t span, const std::uint8_t* prefix, std::size_t plen) {
    for (std::size_t i = 0; i + plen + 4 <= span; ++i) {
        if (std::memcmp(fn + i, prefix, plen) == 0) {
            std::int32_t disp; std::memcpy(&disp, fn + i + plen, 4);
            return const_cast<std::uint8_t*>(fn + i + plen + 4 + disp);
        }
    }
    return nullptr;
}

DWORD WINAPI stats_thread_main(LPVOID) {
    for (;;) {
        ::Sleep(60000);
        const unsigned long long pushes = g_pushes.load(std::memory_order_relaxed);
        if (pushes == g_last_logged_pushes || !g_host || !g_host->log) continue;
        g_last_logged_pushes = pushes;
        char msg[256];
        std::snprintf(msg, sizeof(msg),
            "accel_family_lists: builds=%llu entries=%llu linear-compares-avoided=%llu",
            g_builds.load(std::memory_order_relaxed), pushes,
            g_saved_compares.load(std::memory_order_relaxed));
        g_host->log(kLogInfo, msg);
        if (g_host->report_metric) {
            g_host->report_metric("accel_family_lists.entries", static_cast<double>(pushes));
            g_host->report_metric("accel_family_lists.compares_avoided",
                                  static_cast<double>(g_saved_compares.load(std::memory_order_relaxed)));
        }
    }
}

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)),
    CK3ACCEL_PLUGIN_MAGIC,
    CK3ACCEL_ABI_VERSION,
    "accel_family_lists",
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

    auto* walker = static_cast<std::uint8_t*>(host->scan(kSigWalker));
    auto* addu   = static_cast<std::uint8_t*>(host->scan(kSigAddUnique));
    if (!walker || !addu) {
        host->log(kLogWarn, "accel_family_lists: signature NOT FOUND (walker/add-unique); plugin inert");
        return 0;
    }
    // The array push_back is an ICF-folded function; resolve it from both call sites and
    // require agreement. Sites: add-unique +0x5D, walker +0x206 (1.19.0.6-r20260602).
    void* pb1 = decode_call(addu + 0x5D);
    void* pb2 = decode_call(walker + 0x206);
    if (!pb1 || pb1 != pb2) {
        host->log(kLogWarn, "accel_family_lists: push_back call sites did not agree; plugin inert");
        return 0;
    }
    // Globals referenced by the walker: character db (mov r8,[rip]), null sentinel (mov rbx,[rip]),
    // shared empty array (lea rax,[rip]).
    const std::uint8_t p_db[]    = {0x4C, 0x8B, 0x05};
    const std::uint8_t p_null[]  = {0x48, 0x8B, 0x1D};
    const std::uint8_t p_empty[] = {0x48, 0x8D, 0x05};
    g_db_global   = static_cast<std::uint8_t**>(find_rip_ref(walker, 0x100, p_db, 3));
    g_null_global = static_cast<std::uint8_t**>(find_rip_ref(walker, 0x100, p_null, 3));
    g_empty_array = static_cast<PdxU32Array*>(find_rip_ref(walker, 0x40, p_empty, 3));
    // NOTE: the globals' VALUES (db pointer, null sentinel) are still null this early in
    // process start; they are checked at call time in the detours, not here.
    if (!g_db_global || !g_null_global || !g_empty_array) {
        host->log(kLogWarn, "accel_family_lists: could not decode engine globals; plugin inert");
        g_db_global = g_null_global = nullptr; g_empty_array = nullptr;
        return 0;
    }
    g_push_back = reinterpret_cast<push_back_fn>(pb1);

    if (!host->install_hook(reg->hook_set, addu, reinterpret_cast<void*>(&detour_add_unique),
                            reinterpret_cast<void**>(&g_orig_add_unique)) || !g_orig_add_unique) {
        host->log(kLogWarn, "accel_family_lists: hook on add-unique failed; plugin inert");
        return 0;
    }
    if (!host->install_hook(reg->hook_set, walker, reinterpret_cast<void*>(&detour_walker),
                            reinterpret_cast<void**>(&g_orig_walker)) || !g_orig_walker) {
        host->log(kLogWarn, "accel_family_lists: hook on family walker failed; add-unique-only mode");
        // add-unique alone still helps the helper-level adds; the walker keeps its inline scan.
    }
    g_active.store(true, std::memory_order_release);
    char msg[192];
    std::snprintf(msg, sizeof(msg),
        "accel_family_lists: active (walker=%p add_unique=%p push_back=%p db=%p)",
        static_cast<void*>(walker), static_cast<void*>(addu), pb1, static_cast<void*>(g_db_global));
    host->log(kLogInfo, msg);
    if (ck3accel_has_panels(host)) {
        CK3AccelPanel panel{};
        panel.struct_size = sizeof(panel);
        panel.name = "family list dedup (O(N))";
        panel.enabled = &g_enabled;
        panel.stat_count = 2;
        panel.stat_labels[0] = "builds";           panel.stat_values[0] = reinterpret_cast<const unsigned long long*>(&g_builds);
        panel.stat_labels[1] = "compares avoided"; panel.stat_values[1] = reinterpret_cast<const unsigned long long*>(&g_saved_compares);
        host->register_panel(&panel);
    }
    if (HANDLE th = ::CreateThread(nullptr, 0, &stats_thread_main, nullptr, 0, nullptr)) ::CloseHandle(th);
    return 0;
}
