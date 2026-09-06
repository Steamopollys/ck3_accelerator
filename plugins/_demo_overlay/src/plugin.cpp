// accel_demo_overlay: DEMONSTRATION plugin (opt-in, not shipped).
//
// Draws a Dear ImGui control panel over the running game, proving the framework can put custom UI
// outside the native game UI and take input. It hooks the D3D11 present path (Present is read from a
// throwaway swapchain's vtable, so no ck3.exe RE) and renders each frame. Toggle with F10.
//
// The panel lists every loaded accelerator plugin from the core's registry, showing each one's live
// counters and a checkbox that enables/disables it at runtime. The overlay itself installs no game
// hook beyond rendering; each plugin owns its own toggle. Single-player demo.

#include <ck3accel/core_api.h>

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

#if defined(_WIN32)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif
static_assert(sizeof(void*) == 8, "x64 only");

namespace {
constexpr int kLogInfo = 2, kLogWarn = 3;
const CoreApi* g_host = nullptr;

using present_fn = HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
present_fn g_orig_present = nullptr;

std::atomic<bool> g_active{false};
bool g_show = false;

bool                    g_init = false;
ID3D11Device*           g_device = nullptr;
ID3D11DeviceContext*    g_context = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
HWND                    g_hwnd = nullptr;
WNDPROC                 g_orig_wndproc = nullptr;

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    // Only feed ImGui while the panel is visible; when hidden it touches nothing (no cursor, no input).
    if (g_active.load(std::memory_order_relaxed) && g_init && g_show) {
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        const ImGuiIO& io = ImGui::GetIO();
        if (io.WantCaptureMouse || io.WantCaptureKeyboard) return 1;
    }
    return CallWindowProcW(g_orig_wndproc, h, msg, w, l);
}

void make_rtv(IDXGISwapChain* sc) {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) && back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

void draw_panel() {
    ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
    ImGui::Begin("CK3 Accelerator");
    ImGui::Text("Injected overlay, drawn over the game.  %.0f FPS", ImGui::GetIO().Framerate);
    ImGui::Separator();

    const int n = g_host->panel_count ? g_host->panel_count() : 0;
    if (n == 0) {
        ImGui::TextDisabled("No plugins registered a panel.");
    }
    for (int i = 0; i < n; ++i) {
        const CK3AccelPanel* p = g_host->panel_at(i);
        if (!p || !p->name) continue;
        ImGui::PushID(i);
        if (p->enabled) {
            bool on = *p->enabled != 0;
            if (ImGui::Checkbox(p->name, &on)) *p->enabled = on ? 1 : 0;
        } else {
            ImGui::TextUnformatted(p->name);
        }
        for (int s = 0; s < p->stat_count && s < 4; ++s) {
            if (p->stat_labels[s] && p->stat_values[s])
                ImGui::Text("    %s: %llu", p->stat_labels[s], *p->stat_values[s]);
        }
        ImGui::PopID();
        ImGui::Spacing();
    }

    ImGui::Separator();
    ImGui::TextDisabled("F10 hides this window. Toggling a plugin takes effect immediately.");
    ImGui::End();
}

HRESULT STDMETHODCALLTYPE detour_present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_active.load(std::memory_order_relaxed)) return g_orig_present(sc, sync, flags);

    if (!g_init) {
        if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_device))) && g_device) {
            g_device->GetImmediateContext(&g_context);
            DXGI_SWAP_CHAIN_DESC d{}; sc->GetDesc(&d); g_hwnd = d.OutputWindow;
            make_rtv(sc);
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;  // leave CK3's cursor alone
            ImGui::StyleColorsDark();
            ImGui_ImplWin32_Init(g_hwnd);
            ImGui_ImplDX11_Init(g_device, g_context);
            g_orig_wndproc = reinterpret_cast<WNDPROC>(
                ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndproc)));
            g_init = true;
            if (g_host && g_host->log) g_host->log(kLogInfo, "accel_demo_overlay: D3D11 hooked, overlay ready (press F10)");
        } else {
            return g_orig_present(sc, sync, flags);
        }
    }

    static bool prev_f10 = false;
    const bool f10 = (::GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10 && !prev_f10) g_show = !g_show;
    prev_f10 = f10;

    if (g_show && g_rtv) {
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_panel();
        ImGui::Render();
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    }
    return g_orig_present(sc, sync, flags);
}

void* find_present() {
    WNDCLASSEXW wc{}; wc.cbSize = sizeof(wc); wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr); wc.lpszClassName = L"ck3accel_ovl_dummy";
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr; ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL got{}; const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0};
    void* present = nullptr;
    if (SUCCEEDED(::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            want, 1, D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx)) && sc) {
        void** vtbl = *reinterpret_cast<void***>(sc);
        present = vtbl[8];   // IDXGISwapChain::Present
    }
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (sc)  sc->Release();
    if (hwnd) ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return present;
}

bool conf_enabled() {
    HMODULE self = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&conf_enabled), &self);
    wchar_t buf[MAX_PATH]; DWORD n = ::GetModuleFileNameW(self, buf, MAX_PATH); if (!n || n >= MAX_PATH) return false;
    std::wstring p(buf, n); auto l = p.find_last_of(L"\\/"); if (l == std::wstring::npos) return false;
    std::wstring pl = p.substr(0, l); auto pv = pl.find_last_of(L"\\/"); if (pv == std::wstring::npos) return false;
    FILE* f = nullptr; if (_wfopen_s(&f, (pl.substr(0, pv) + L"\\overlay.conf").c_str(), L"rb") != 0 || !f) return false;
    char t[256]; size_t r = std::fread(t, 1, sizeof(t) - 1, f); t[r] = 0; std::fclose(f);
    std::string s(t); return s.find("overlay=true") != std::string::npos || s.find("overlay = true") != std::string::npos;
}

const CK3AccelPluginInfo kInfo = {
    static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)), CK3ACCEL_PLUGIN_MAGIC, CK3ACCEL_ABI_VERSION,
    "accel_demo_overlay", "0.1.0", "1.19.0.6", "1.19.0.6", CK3ACCEL_MODE_SP,
};
}  // namespace

PLUGIN_EXPORT const CK3AccelPluginInfo* CK3Accel_Query(uint32_t v) { (void)v; return &kInfo; }

PLUGIN_EXPORT int CK3Accel_Init(const CoreApi* host, CK3AccelRegistrar* reg) {
    g_host = host;
    if (!host || !host->log || !host->install_hook || !reg) return 1;
    if (!conf_enabled()) { host->log(kLogInfo, "accel_demo_overlay: disabled (set overlay=true in overlay.conf); inert"); return 0; }
    if (!ck3accel_has_panels(host)) host->log(kLogWarn, "accel_demo_overlay: core has no panel registry; panel will be empty");
    void* present = find_present();
    if (!present) { host->log(kLogWarn, "accel_demo_overlay: could not resolve D3D11 Present; inert"); return 0; }
    if (!(host->install_hook(reg->hook_set, present, reinterpret_cast<void*>(&detour_present),
                             reinterpret_cast<void**>(&g_orig_present)) != nullptr && g_orig_present)) {
        host->log(kLogWarn, "accel_demo_overlay: Present hook failed; inert"); return 0;
    }
    g_active.store(true, std::memory_order_release);
    host->log(kLogInfo, "accel_demo_overlay: active (press F10 in-game to toggle the panel)");
    return 0;
}
