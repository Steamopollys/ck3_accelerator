// ck3accel_proxy_winmm: a winmm.dll that loads ck3accel_core.dll. No-rename injection.
//
// CK3 imports winmm.dll but doesn't ship it, and winmm isn't a KnownDLL or an API-set on Win11
// (that redirection is what killed the version.dll proxy), so our copy in binaries\ loads first
// with nothing to rename. Every export tail-jumps (winmm_thunks.asm) to the real System32\winmm.dll,
// whose pointers we fill below; then we load the core. Game runs stock if the core is missing.
#include <windows.h>

#include <filesystem>
#include <vector>

#include "winmm_forwards.inc"   // g_winmm_real[] + kWinmmFwds[] (generated from the real exports)

namespace {
    HMODULE g_core = nullptr;
    HMODULE g_real = nullptr;

    std::filesystem::path self_directory() {
        std::vector<wchar_t> buf(MAX_PATH);
        HMODULE me = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&self_directory), &me);
        DWORD len = 0;
        for (;;) {
            len = GetModuleFileNameW(me, buf.data(), static_cast<DWORD>(buf.size()));
            if (len == 0) return {};
            if (len < buf.size()) break;
            buf.resize(buf.size() * 2);
        }
        return std::filesystem::path(buf.begin(), buf.begin() + len).parent_path();
    }

    std::filesystem::path system_winmm() {
        std::vector<wchar_t> buf(MAX_PATH);
        UINT len = GetSystemDirectoryW(buf.data(), static_cast<UINT>(buf.size()));
        if (len == 0 || len > buf.size()) return {};
        return std::filesystem::path(std::wstring(buf.data(), len)) / L"winmm.dll";
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        // Real winmm from System32 (full path), so the thunks resolve before we return.
        g_real = LoadLibraryW(system_winmm().c_str());
        if (g_real) {
            for (int i = 0; i < WINMM_FWD_COUNT; ++i) {
                const WinmmFwd& f = kWinmmFwds[i];
                FARPROC p = f.name ? GetProcAddress(g_real, f.name)
                                   : GetProcAddress(g_real, MAKEINTRESOURCEA(f.ordinal));
                g_winmm_real[i] = reinterpret_cast<void*>(p);
            }
        }
        // Then the core (best-effort).
        g_core = LoadLibraryW((self_directory() / L"ck3accel_core.dll").c_str());
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_core) { FreeLibrary(g_core); g_core = nullptr; }
        if (g_real) { FreeLibrary(g_real); g_real = nullptr; }
    }
    return TRUE;
}
