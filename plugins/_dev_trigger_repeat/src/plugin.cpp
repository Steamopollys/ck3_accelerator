// accel_trigger_repeat: DEVELOPER-ONLY trigger repeat/consistency probe (NOT SHIPPED).
//
// Measures per daily tick the trigger evaluator's (scope,trigger-id) repeat rate and result
// consistency, to decide GO/NO-GO on the trigger cache. Observe-only: returns each trigger's
// result UNCHANGED, never caches or alters anything. SP-only; off until trigger_repeat.conf
// enables it. See README.md.

#include <ck3accel/core_api.h>
#include <ck3accel/repeat_table.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif

static_assert(sizeof(void*) == 8, "accel_trigger_repeat is x64-only");

namespace {
constexpr int kLogInfo = 2;
constexpr int kLogWarn = 3;

const CoreApi* g_host = nullptr;
std::uintptr_t g_module_base = 0;
bool g_enabled = false;

void* g_addr_trigger = nullptr;  // 0x0334C550
void* g_addr_tick    = nullptr;  // 0x0204EFE0

// Per-tick measurement tables, one pair per thread (trigger eval runs on worker threads).
// kCap = 2^21 slots (~32 MiB/table); the table's overflow counter makes saturation explicit.
constexpr std::size_t kCap = 1u << 21;
struct Shard {
    ck3accel::RepeatTable<kCap> ent;   // content key: stable scope identity + trigger_id
    ck3accel::RepeatTable<kCap> ptr;   // fallback pointer key: rcx + trigger_id (lower bound)
};
thread_local Shard* t_shard = nullptr;

std::mutex g_shards_mtx;
std::vector<Shard*> g_shards;          // every thread's shard, for the tick-boundary merge

Shard* ensure_shard() {
    if (!t_shard) {
        // ONE-TIME per thread (NOT the per-call hot path). Intentionally never freed:
        // this is a dev probe with no shutdown export; the worker threads + their shards
        // live to process exit, which reclaims them.
        t_shard = new Shard();
        std::lock_guard<std::mutex> lk(g_shards_mtx);
        g_shards.push_back(t_shard);
    }
    return t_shard;
}

// Stable scope identity from the CScopeObjectReference embedded at scope+0x10: 4 x uint64 at scope+0x10..+0x28 plus a
// 1-byte type discriminator at scope+0x30, folded FNV-1a-style. This is the game's own ref
// identity (hashed at ck3.exe 0x0096EDD0), NOT the rcx pointer (a reallocating wrapper).
inline std::uint64_t scope_content_id(void* scope) {
    const auto* base = reinterpret_cast<const unsigned char*>(scope);
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (int j = 0; j < 4; ++j) {
        std::uint64_t w;
        std::memcpy(&w, base + 0x10 + j * 8, sizeof(w));   // unaligned-safe
        h = (h ^ w) * 0x100000001b3ull;
    }
    h = (h ^ static_cast<std::uint64_t>(base[0x30])) * 0x100000001b3ull;
    return h;
}

// Combine a scope identity (pointer or content hash) with the trigger id without losing
// bits (the table re-hashes with splitmix64 anyway).
inline std::uint64_t make_key(std::uint64_t scope_id, std::uint32_t trig_id) {
    return scope_id ^ (static_cast<std::uint64_t>(trig_id) * 0x9E3779B97F4A7C15ull);
}

using trigger_fn = char (*)(void* scope, void* node, char silent);
trigger_fn g_orig_trigger = nullptr;

using tick_fn = int (*)(void* gamestate);
tick_fn  g_orig_tick = nullptr;
FILE*    g_csv = nullptr;
bool     g_csv_open_attempted = false;
std::uint64_t g_day_index = 0;
// Sim-thread-only merge targets (each ~32 MiB); the day-boundary merge folds all shards in.
ck3accel::RepeatTable<kCap>* g_merge_ent = nullptr;
ck3accel::RepeatTable<kCap>* g_merge_ptr = nullptr;

// OBSERVE-ONLY: record (scope,trigger) + result, then run the real trigger and return its
// result UNCHANGED. Nothing is cached; nothing is altered.
char detour_trigger(void* scope, void* node, char silent) {
    Shard* sh = ensure_shard();
    // Trigger-TYPE id = the engine's own dense registry index: r12 = *(void**)node, then
    // id = *(uint16_t*)r12. Verified by full disassembly of 0x0334C550: bounds-checked
    // (cmp [registry+0xc], id) and indexed *0x50 into the trigger registry from the singleton
    // 0x033C5200, so it uniquely identifies the trigger TYPE. `[node]` is a valid pointer (the
    // evaluator itself does `mov r12,[rdi]; movzx eax,word[r12]`), so the double-deref is safe.
    // (scope+0x8 is an entity-side field the evaluator never reads; keying on it collapsed
    // distinct triggers on one scope and inflated the measured inconsistency. Do NOT use it.)
    const std::uint32_t trig_id =
        node ? *reinterpret_cast<const std::uint16_t*>(*reinterpret_cast<void**>(node)) : 0u;
    const char rc = g_orig_trigger(scope, node, silent);     // run the real trigger
    const std::uint8_t res = static_cast<std::uint8_t>(rc);  // full byte: a non-bool result
                                                             // then surfaces as an inconsistency
    sh->ptr.record(make_key(reinterpret_cast<std::uint64_t>(scope), trig_id), res);  // pointer key
    if (scope) sh->ent.record(make_key(scope_content_id(scope), trig_id), res);      // content key
    return rc;   // observe-only: unchanged
}

const char* const kSigTrigger =
    "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 "
    "48 8D A8 C8 FD FF FF 48 81 EC 10 03 00 00";
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

void read_conf() {
    const std::wstring install = self_dll_directory();
    if (install.empty()) return;
    const std::wstring path = install + L"\\trigger_repeat.conf";
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return;
    char buf[1024]; std::string txt;
    std::size_t n; while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) txt.append(buf, n);
    std::fclose(f);
    std::size_t pos = 0;
    while (pos <= txt.size()) {
        const std::size_t nl = txt.find('\n', pos);
        std::string line = txt.substr(pos, (nl == std::string::npos ? txt.size() : nl) - pos);
        pos = (nl == std::string::npos) ? txt.size() + 1 : nl + 1;
        const std::size_t h = line.find('#'); if (h != std::string::npos) line = line.substr(0, h);
        const std::size_t eq = line.find('='); if (eq == std::string::npos) continue;
        auto trim = [](std::string s){ const char* w=" \t\r\n"; auto a=s.find_first_not_of(w);
            if(a==std::string::npos) return std::string(); auto b=s.find_last_not_of(w);
            return s.substr(a,b-a+1); };
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k == "trigger_repeat") g_enabled = (v=="1"||v=="true"||v=="yes"||v=="on");
    }
}

void ensure_csv_open() {
    if (g_csv || g_csv_open_attempted) return;
    g_csv_open_attempted = true;
    const std::wstring install = self_dll_directory();
    if (install.empty()) return;
    const std::wstring logs = install + L"\\logs";
    ::CreateDirectoryW(logs.c_str(), nullptr);
    const std::wstring path = logs + L"\\trigger_repeat.csv";
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    const bool existed = (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
    if (_wfopen_s(&g_csv, path.c_str(), L"a") != 0 || !g_csv) { g_csv = nullptr; return; }
    if (!existed)
        std::fprintf(g_csv, "day_index,total_evals,distinct_keys,repeat_pct,"
            "inconsistent_keys,inconsistency_pct,distinct_keys_ptr,repeat_pct_ptr,overflow\n");
    std::fflush(g_csv);
}

int detour_tick(void* gamestate) {
    const int rc = g_orig_tick(gamestate);   // run the day FIRST (observe-only)

    // Merge all per-thread shards into the sim-thread merge tables, then tally. Each shard is
    // reset in the SAME locked pass right after merging, so no record is merged-then-lost or
    // counted in the wrong day. Reading a shard unlocked here is safe: the day's parallel trigger
    // work has finished by the time UpdateTurnTick returns (workers quiescent at the day boundary).
    // The per-thread tables serve the multi-thread record path; the merge is the single-thread
    // day-boundary read.
    g_merge_ent->reset();
    g_merge_ptr->reset();
    {
        std::lock_guard<std::mutex> lk(g_shards_mtx);
        for (Shard* sh : g_shards) {
            g_merge_ent->merge_from(sh->ent);
            g_merge_ptr->merge_from(sh->ptr);
            sh->ent.reset();   // reset under the same lock -> no lost-record window
            sh->ptr.reset();
        }
    }
    const ck3accel::RepeatTally e = g_merge_ent->tally();
    const ck3accel::RepeatTally p = g_merge_ptr->tally();
    const double rep  = e.total ? 100.0 * double(e.total - e.distinct) / double(e.total) : 0.0;
    const double inc  = e.repeated_keys ? 100.0 * double(e.inconsistent_keys) / double(e.repeated_keys) : 0.0;
    const double repp = p.total ? 100.0 * double(p.total - p.distinct) / double(p.total) : 0.0;

    ensure_csv_open();
    if (g_csv) {
        std::fprintf(g_csv, "%llu,%llu,%llu,%.3f,%llu,%.4f,%llu,%.3f,%llu\n",
            (unsigned long long)g_day_index, (unsigned long long)e.total,
            (unsigned long long)e.distinct, rep, (unsigned long long)e.inconsistent_keys, inc,
            (unsigned long long)p.distinct, repp, (unsigned long long)(e.overflow + p.overflow));
        std::fflush(g_csv);
    }
    if (g_host && g_host->log && e.total) {
        char msg[192];
        std::snprintf(msg, sizeof(msg),
            "trigger_repeat day %llu: %llu evals, %llu distinct, repeat=%.1f%%, "
            "inconsistency=%.3f%% (ptr repeat=%.1f%%, overflow=%llu)",
            (unsigned long long)g_day_index, (unsigned long long)e.total,
            (unsigned long long)e.distinct, rep, inc, repp,
            (unsigned long long)(e.overflow + p.overflow));
        g_host->log(kLogInfo, msg);
    }
    if (g_host && g_host->report_metric) {
        g_host->report_metric("accel_trigger_repeat.repeat_pct", rep);
        g_host->report_metric("accel_trigger_repeat.inconsistency_pct", inc);
    }
    ++g_day_index;
    return rc;   // observe-only: unchanged
}

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)),  // struct_size (FIRST)
    CK3ACCEL_PLUGIN_MAGIC,
    CK3ACCEL_ABI_VERSION,
    "accel_trigger_repeat",                             // name (allowlist key)
    "0.1.0",
    "any",
    "any",
    CK3ACCEL_MODE_SP,                                   // SP-only dev tool
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
    read_conf();
    if (!g_enabled) {
        host->log(kLogInfo, "accel_trigger_repeat: disabled (set trigger_repeat=true in "
                            "trigger_repeat.conf); inert");
        return 0;
    }
    g_addr_trigger = host->scan(kSigTrigger);
    g_addr_tick    = host->scan(kSigTick);
    if (!g_addr_trigger || !g_addr_tick) {
        host->log(kLogWarn, "accel_trigger_repeat: a signature was NOT FOUND; probe inert");
        g_addr_trigger = g_addr_tick = nullptr;
        return 0;  // soft-fail
    }
    host->install_hook(reg->hook_set, g_addr_trigger,
                       reinterpret_cast<void*>(&detour_trigger),
                       reinterpret_cast<void**>(&g_orig_trigger));
    host->log(kLogInfo, "accel_trigger_repeat: hooked trigger evaluator");
    g_merge_ent = new ck3accel::RepeatTable<kCap>();
    g_merge_ptr = new ck3accel::RepeatTable<kCap>();
    host->install_hook(reg->hook_set, g_addr_tick,
                       reinterpret_cast<void*>(&detour_tick),
                       reinterpret_cast<void**>(&g_orig_tick));
    host->log(kLogInfo, "accel_trigger_repeat: hooked UpdateTurnTick; measuring per-day");
    return 0;
}
