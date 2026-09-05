#include "paths.h"
#include <windows.h>
#include <atomic>
#include <vector>

namespace ck3accel {

namespace {
    std::atomic<HMODULE> g_self_module{nullptr};

    std::filesystem::path module_path(HMODULE m) {
        std::vector<wchar_t> buf(MAX_PATH);
        DWORD len = 0;
        for (;;) {
            len = GetModuleFileNameW(m, buf.data(), static_cast<DWORD>(buf.size()));
            if (len == 0) {
                return {};
            }
            if (len < buf.size()) {
                return std::filesystem::path(buf.begin(), buf.begin() + len);
            }
            buf.resize(buf.size() * 2);
        }
    }
}

void record_self_module(void* hmodule) {
    g_self_module.store(static_cast<HMODULE>(hmodule), std::memory_order_release);
}

std::filesystem::path dll_directory() {
    HMODULE m = g_self_module.load(std::memory_order_acquire);
    if (!m) {
        // unit-test fallback (DllMain didn't run): ask the loader which module
        // holds this function's address.
        if (!GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&dll_directory),
                &m)) {
            return {};
        }
    }
    auto p = module_path(m);
    return p.empty() ? std::filesystem::path{} : p.parent_path();
}

std::filesystem::path install_directory() {
    return dll_directory();
}

std::filesystem::path config_path() {
    return install_directory() / "config.toml";
}

std::filesystem::path log_directory() {
    return install_directory() / "logs";
}

std::filesystem::path versions_json_path() {
    return install_directory() / "versions.json";
}

std::filesystem::path signatures_directory() {
    return install_directory() / "sigs";
}

} // namespace ck3accel
