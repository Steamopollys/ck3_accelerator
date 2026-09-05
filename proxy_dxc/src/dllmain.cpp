// ck3accel_proxy_dxc: dxcompiler.dll proxy that loads ck3accel_core.dll.
//
// Install: rename the game's binaries\dxcompiler.dll to dxcompiler_orig.dll and put this
// DLL in its place as dxcompiler.dll. CK3 imports dxcompiler.dll statically, so this runs
// at process start (same timing as the old version.dll proxy). Both real exports are
// forwarded BY HAND to the renamed original (loaded by full path from our own directory):
// no PE forwarders, no search-path games.
//
// Fail-safe: if dxcompiler_orig.dll is missing, the exports return E_FAIL-style HRESULTs
// (the game only needs DXC for the DX12 shader path); if the core is missing the game just
// runs stock. Steam "verify integrity" restores the original dxcompiler.dll over this file,
// which simply turns the accelerator off.
#include <windows.h>

#include <filesystem>
#include <vector>

namespace {
    using DxcCreateInstance_t  = HRESULT (WINAPI*)(const GUID* rclsid, const GUID* riid, void** ppv);
    using DxcCreateInstance2_t = HRESULT (WINAPI*)(void* pMalloc, const GUID* rclsid, const GUID* riid, void** ppv);

    HMODULE g_core = nullptr;
    HMODULE g_real = nullptr;
    DxcCreateInstance_t  g_create  = nullptr;
    DxcCreateInstance2_t g_create2 = nullptr;

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
}

extern "C" __declspec(dllexport) HRESULT WINAPI DxcCreateInstance(const GUID* rclsid, const GUID* riid, void** ppv) {
    if (!g_create) return static_cast<HRESULT>(0x80004005L);   // E_FAIL
    return g_create(rclsid, riid, ppv);
}

extern "C" __declspec(dllexport) HRESULT WINAPI DxcCreateInstance2(void* pMalloc, const GUID* rclsid, const GUID* riid, void** ppv) {
    if (!g_create2) return static_cast<HRESULT>(0x80004005L);  // E_FAIL
    return g_create2(pMalloc, rclsid, riid, ppv);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        const auto dir = self_directory();
        // FIRST: the real compiler, by full path, so the exports work before we return.
        g_real = LoadLibraryW((dir / L"dxcompiler_orig.dll").c_str());
        if (g_real) {
            g_create  = reinterpret_cast<DxcCreateInstance_t>(GetProcAddress(g_real, "DxcCreateInstance"));
            g_create2 = reinterpret_cast<DxcCreateInstance2_t>(GetProcAddress(g_real, "DxcCreateInstance2"));
        }
        // THEN: our core. Best-effort; the game runs stock if it is absent.
        g_core = LoadLibraryW((dir / L"ck3accel_core.dll").c_str());
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_core) { FreeLibrary(g_core); g_core = nullptr; }
        if (g_real) { FreeLibrary(g_real); g_real = nullptr; }
    }
    return TRUE;
}
