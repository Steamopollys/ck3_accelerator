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

// Trigger eval comes through the core's shared trigger service (so this coexists with tick_cache).

std::atomic<bool> g_active{false};
int  g_enabled = 0;                          // runtime on/off; OFF until toggled from the overlay panel
char g_force = 1;                            // value we force the matched trigger to return
char g_class[128] = "CHasTraitTrigger";      // trigger class to override (matched as a demangled-name substring)
std::atomic<unsigned long long> g_overrides{0};
std::atomic<unsigned long long> g_seen{0};        // diagnostic: trigger evals seen while enabled
char g_first_class[256] = {0};                    // diagnostic: first RTTI name we resolved
std::atomic<bool> g_first_logged{false};

// ---- per-vtable decision cache: 0 unknown, 1 override, 2 leave-alone -------------------------
constexpr std::size_t kSlots = 1u << 13;
struct Dec { std::atomic<std::uint64_t> vt{0}; std::atomic<std::uint8_t> d{0}; };
Dec g_dec[kSlots];

std::uint8_t decision_for(std::uint64_t vt) {
    if (!vt) return 2;
    std::size_t i = static_cast<std::size_t>((vt >> 4) * 0x9E3779B97F4A7C15ull >> 50) & (kSlots - 1);
    for (std::size_t p = 0; p < 64; ++p) {
        std::uint64_t cur = g_dec[i].vt.load(std::memory_order_relaxed);
        if (cur == vt) return g_dec[i].d.load(std::memory_order_relaxed);
        if (cur == 0) {
            char nm[256] = {0};
            const bool named = ck3accel::rtti::class_name({g_module_base, g_image_size}, vt, nm, sizeof(nm));
            if (named && !g_first_logged.load(std::memory_order_relaxed)) {   // capture one sample name
                std::strncpy(g_first_class, nm, sizeof(g_first_class) - 1);
                g_first_logged.store(true, std::memory_order_relaxed);
            }
            const std::uint8_t d = (named && std::strstr(nm, g_class) != nullptr) ? 1 : 2;
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

char override_handler(void* node, void* ctx, unsigned char skip, ck3accel_trigger_next next, void* next_ctx, void*) {
    if (!g_enabled || !g_active.load(std::memory_order_relaxed) || !node) return next(node, ctx, skip, next_ctx);
    g_seen.fetch_add(1, std::memory_order_relaxed);
    if (decision_for(ck3accel::rtti::read_vtable(node)) == 1) {   // matched class -> override the engine's answer
        g_overrides.fetch_add(1, std::memory_order_relaxed);
        return g_force;
    }
    return next(node, ctx, skip, next_ctx);
}

// Diagnostic heartbeat: triggers seen while enabled, how many matched the class, and one sample name.
DWORD WINAPI diag_thread(LPVOID) {
    bool logged_class = false;
    unsigned long long prev = 0;
    for (;;) {
        ::Sleep(5000);
        if (!g_active.load(std::memory_order_relaxed) || !g_host || !g_host->log) continue;
        if (!logged_class && g_first_logged.load(std::memory_order_relaxed)) {
            char m[320]; std::snprintf(m, sizeof(m), "accel_demo: matching class '%s'; first RTTI name seen '%s'", g_class, g_first_class);
            g_host->log(kLogInfo, m); logged_class = true;
        }
        const unsigned long long seen = g_seen.load(), ov = g_overrides.load();
        const unsigned long long d = ov - prev; prev = ov;
        if (seen == 0 && ov == 0) continue;
        char msg[192];
        std::snprintf(msg, sizeof(msg), "accel_demo: seen=%llu overrode=%llu (+%llu) -> %s",
                      seen, ov, d, g_force ? "true" : "false");
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
    if (!host || !host->log || !reg) return 1;
    if (!read_conf()) { host->log(kLogInfo, "accel_demo: disabled (set demo=true in demo.conf); inert"); return 0; }
    g_module_base = reinterpret_cast<std::uintptr_t>(::GetModuleHandleW(nullptr));
    { MODULEINFO mi{}; if (::GetModuleInformation(::GetCurrentProcess(), ::GetModuleHandleW(nullptr), &mi, sizeof(mi))) g_image_size = mi.SizeOfImage; }
    if (!ck3accel_has_trigger_service(host) || !host->ensure_trigger_service()) {
        host->log(kLogWarn, "accel_demo: shared trigger service unavailable; inert"); return 0;
    }
    host->register_trigger_handler(&override_handler, nullptr, /*priority=*/100);  // outermost (before caches)
    g_active.store(true, std::memory_order_release);
    if (HANDLE t = ::CreateThread(nullptr, 0, &diag_thread, nullptr, 0, nullptr)) ::CloseHandle(t);
    if (ck3accel_has_panels(host)) {
        CK3AccelPanel panel{};
        panel.struct_size = sizeof(panel);
        panel.name = "has_trait override (demo)";
        panel.enabled = &g_enabled;
        panel.stat_count = 2;
        panel.stat_labels[0] = "overrides"; panel.stat_values[0] = reinterpret_cast<const unsigned long long*>(&g_overrides);
        panel.stat_labels[1] = "seen";      panel.stat_values[1] = reinterpret_cast<const unsigned long long*>(&g_seen);
        host->register_panel(&panel);
    }
    host->log(kLogInfo, "accel_demo: loaded (off; enable from the overlay panel). Changes gameplay, not checksum-safe.");
    return 0;
}
