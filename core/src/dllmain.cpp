#include <windows.h>
#include "core_init.h"
#include "paths.h"

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID /*reserved*/) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            ck3accel::record_self_module(hModule);
            ck3accel::core_init();
            break;
        case DLL_PROCESS_DETACH:
            ck3accel::core_shutdown();
            break;
    }
    return TRUE;
}
