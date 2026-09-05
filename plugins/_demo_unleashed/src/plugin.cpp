// accel_demo: DEMONSTRATION plugin (opt-in, NOT shipped).
//
// Shows the framework can change GAMEPLAY, not just speed. Hooks the script trigger evaluator and,
// for one chosen trigger class, overrides the answer the engine's own C++ returns (something no
// script mod can do). Default: force every `has_trait` check to return true; pick another
// class/value in demo.conf. It only changes a return value (no memory or scope writes), so it can't
// corrupt a save, and the kill-switch or uninstalling reverts it instantly.
//
// WARNING: NOT checksum-neutral. It changes game state, so it disables Ironman achievements and
// desyncs multiplayer. Use it only on a throwaway single-player save, never on one you care about.

#include <ck3accel/core_api.h>

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

// Same trigger-evaluator entry the tick cache hooks.
const char* const kSigTrigger =
    "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D A8 C8 FD FF FF";

using trigger_fn = char (*)(void* node, std::uint8_t* ctx, std::uint8_t skip);
trigger_fn g_orig = nullptr;

std::atomic<bool> g_active{false};
char g_force = 1;                            // value we force the matched trigger to return
char g_class[128] = "CHasTraitTrigger";      // trigger class (demangled-name prefix) to override
std::atomic<unsigned long long> g_overrides{0};
unsigned long long g_prev = 0;

// ---- per-vtable decision cache: 0 unknown, 1 override, 2 leave-alone -------------------------
constexpr std::size_t kSlots = 1u << 13;
struct Dec { std::atomic<std::uint64_t> vt{0}; std::atomic<std::uint8_t> d{0}; };
Dec g_dec[kSlots];

std::uint64_t read_vtable(void* node) {
    __try { return *reinterpret_cast<std::uint64_t*>(node); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
inline bool in_image(const void* q, std::size_t n = 8) {
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(q);
    return g_module_base && a >= g_module_base && a + n <= g_module_base + g_image_size;
}
// Resolve a node's RTTI class name (SEH-guarded), same walk the tick cache uses.
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

std::uint8_t decision_for(std::uint64_t vt) {
    if (!vt) return 2;
    std::size_t i = static_cast<std::size_t>((vt >> 4) * 0x9E3779B97F4A7C15ull >> 50) & (kSlots - 1);
    for (std::size_t p = 0; p < 64; ++p) {
        std::uint64_t cur = g_dec[i].vt.load(std::memory_order_relaxed);
        if (cur == vt) return g_dec[i].d.load(std::memory_order_relaxed);
        if (cur == 0) {
            char nm[256] = {0};
            const std::size_t n = std::strlen(g_class);
            const std::uint8_t d = (class_name(vt, nm, sizeof(nm)) && std::strncmp(nm, g_class, n) == 0) ? 1 : 2;
            std::uint64_t exp = 0;
            if (g_dec[i].vt.compare_exchange_strong(exp, vt, std::memory_order_relaxed)) {
                g_dec[i].d.store(d, std::memory_order_relaxed); return d;
            }
            if (g_dec[i].vt.load(std::memory_order_relaxed) == vt) return g_dec[i].d.load(std::memory_order_relaxed);
        }
        i = (i + 1) & (kSlots - 1);
    }
    return 2;
}

char detour(void* node, std::uint8_t* ctx, std::uint8_t skip) {
    if (!g_active.load(std::memory_order_relaxed) || !node) return g_orig(node, ctx, skip);
    if (decision_for(read_vtable(node)) == 1) {          // matched class -> override the engine's answer
        g_overrides.fetch_add(1, std::memory_order_relaxed);
        return g_force;
    }
    return g_orig(node, ctx, skip);
}

// Low-frequency proof-of-life: reports how many engine evaluations we overrode.
DWORD WINAPI diag_thread(LPVOID) {
    for (;;) {
        ::Sleep(5000);
        if (!g_active.load(std::memory_order_relaxed) || !g_host || !g_host->log) continue;
        const unsigned long long n = g_overrides.load();
        const unsigned long long d = n - g_prev; g_prev = n;
        if (d == 0) continue;
        char msg[160];
        std::snprintf(msg, sizeof(msg), "accel_demo: overrode %llu '%s' checks -> %s (total %llu)",
                      d, g_class, g_force ? "true" : "false", n);
        g_host->log(kLogInfo, msg);
    }
}

// demo.conf next to ck3accel_core.dll: `demo = true`, optional `class = <ClassPrefix>`, `force = true|false`.
bool read_conf() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&read_conf), &self);
    wchar_t buf[MAX_PATH]; DWORD n = ::GetModuleFileNameW(self, buf, MAX_PATH); if (!n || n >= MAX_PATH) return false;
    std::wstring p(buf, n); auto l = p.find_last_of(L"\\/"); if (l == std::wstring::npos) return false;
    std::wstring pl = p.substr(0, l); auto pv = pl.find_last_of(L"\\/"); if (pv == std::wstring::npos) return false;
    FILE* f = nullptr; if (_wfopen_s(&f, (pl.substr(0, pv) + L"\\demo.conf").c_str(), L"rb") != 0 || !f) return false;
    char t[512]; size_t r = std::fread(t, 1, sizeof(t) - 1, f); t[r] = 0; std::fclose(f);
    std::string s(t);
    const bool on = s.find("demo=true") != std::string::npos || s.find("demo = true") != std::string::npos;
    if (!on) return false;
    if (s.find("force=false") != std::string::npos || s.find("force = false") != std::string::npos) g_force = 0;
    // optional class override: `class = SomePrefix`
    auto k = s.find("class");
    if (k != std::string::npos) {
        auto eq = s.find('=', k);
        if (eq != std::string::npos) {
            std::size_t i = eq + 1; while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
            std::size_t j = i; while (j < s.size() && s[j] != '\r' && s[j] != '\n' && s[j] != ' ' && s[j] != '#') ++j;
            if (j > i) { std::string c = s.substr(i, j - i); if (!c.empty() && c.size() < sizeof(g_class)) std::strcpy(g_class, c.c_str()); }
        }
    }
    return true;
}

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)), CK3ACCEL_PLUGIN_MAGIC, CK3ACCEL_ABI_VERSION,
    "accel_demo", "0.1.0", "1.19.0.6", "1.19.0.6", CK3ACCEL_MODE_SP,   // SP only, not Ironman-safe
};
}  // namespace

PLUGIN_EXPORT const CK3AccelPluginInfo* CK3Accel_Query(uint32_t v) { (void)v; return &kInfo; }

PLUGIN_EXPORT int CK3Accel_Init(const CoreApi* host, CK3AccelRegistrar* reg) {
    g_host = host;
    if (!host || !host->log || !host->scan || !host->install_hook || !reg) return 1;
    if (!read_conf()) { host->log(kLogInfo, "accel_demo: disabled (set demo=true in demo.conf); inert"); return 0; }
    g_module_base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    { MODULEINFO mi{}; if (::GetModuleInformation(::GetCurrentProcess(), ::GetModuleHandleW(nullptr), &mi, sizeof(mi))) g_image_size = mi.SizeOfImage; }
    void* trig = host->scan(kSigTrigger);
    if (!trig) { host->log(kLogWarn, "accel_demo: trigger signature NOT FOUND (conflicts with accel_tick_cache? disable it); inert"); return 0; }
    if (!(host->install_hook(reg->hook_set, trig, reinterpret_cast<void*>(&detour), reinterpret_cast<void**>(&g_orig)) != nullptr && g_orig)) {
        host->log(kLogWarn, "accel_demo: trigger hook failed; inert"); return 0;
    }
    g_active.store(true, std::memory_order_release);
    if (HANDLE t = ::CreateThread(nullptr, 0, &diag_thread, nullptr, 0, nullptr)) ::CloseHandle(t);
    char msg[192];
    std::snprintf(msg, sizeof(msg), "accel_demo: ACTIVE — overriding '%s' -> %s (DEMO: changes gameplay, breaks Ironman achievements)",
                  g_class, g_force ? "true" : "false");
    host->log(kLogWarn, msg);
    return 0;
}
