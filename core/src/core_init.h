#pragma once

namespace ck3accel {

// from DllMain DLL_PROCESS_ATTACH. init logger, load config, detect CK3 version,
// log it. true on success; on failure logs and stays loaded to forward exports
// (never self-unloads).
bool core_init();

// from DllMain DLL_PROCESS_DETACH.
void core_shutdown();

} // namespace ck3accel
