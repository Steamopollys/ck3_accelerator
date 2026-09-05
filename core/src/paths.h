#pragma once
#include <filesystem>

namespace ck3accel {

// dir containing ck3accel_core.dll.
std::filesystem::path dll_directory();

// install root. same as dll_directory() for now; separate so a future multi-DLL
// layout can change one without the other.
std::filesystem::path install_directory();

// install_directory() / "config.toml"
std::filesystem::path config_path();

// install_directory() / "logs"
std::filesystem::path log_directory();

// install_directory() / "versions.json"
std::filesystem::path versions_json_path();

// install_directory() / "sigs"
std::filesystem::path signatures_directory();

// stash our HMODULE at attach so dll_directory() can resolve it. from DllMain.
void record_self_module(void* hmodule);

} // namespace ck3accel
