// sample_noop: smallest valid CK3 Accelerator plugin.
// Exercises the loader's LoadLibrary -> Query -> gate -> Init path with zero hooks.
#include <ck3accel/core_api.h>

#if defined(_WIN32)
#  define PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#  define PLUGIN_EXPORT extern "C"
#endif

namespace {
    const CK3AccelPluginInfo kInfo = {
        static_cast<uint32_t>(sizeof(CK3AccelPluginInfo)), // struct_size
        CK3ACCEL_PLUGIN_MAGIC,          // magic
        CK3ACCEL_ABI_VERSION,           // required_abi
        "sample_noop",                  // name (must match [plugins] key)
        "0.1.0",                        // semver
        "any",                          // min_game_version
        "any",                          // max_game_version
        CK3ACCEL_MODE_SP | CK3ACCEL_MODE_IRONMAN | CK3ACCEL_MODE_MULTIPLAYER, // mode_flags
    };
}

PLUGIN_EXPORT const CK3AccelPluginInfo* CK3Accel_Query(uint32_t host_abi_version) {
    (void)host_abi_version;
    return &kInfo;
}

PLUGIN_EXPORT int CK3Accel_Init(const CoreApi* host, CK3AccelRegistrar* reg) {
    (void)reg;  // we register no hooks
    if (host && host->log) {
        host->log(/*Info=*/2, "sample_noop: loaded; no hooks installed");
    }
    return 0;  // success
}
