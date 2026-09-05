# ck3accel_proxy_dxc: injection vehicle #2 (2026-09-02, user-approved).
#
# Win11 25H2 won't resolve an app-local version.dll, so we inject via CK3's own
# dxcompiler.dll (DirectX Shader Compiler in binaries\, two exports, none by that
# name in System32). Game's copy is renamed dxcompiler_orig.dll; this DLL forwards
# both exports to it by hand (no PE forwarders, same lesson as the version.dll proxy).
add_library(ck3accel_proxy_dxc SHARED
    src/dllmain.cpp
)
target_link_options(ck3accel_proxy_dxc PRIVATE
    /DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/dxc_forwards.def
)
set_target_properties(ck3accel_proxy_dxc PROPERTIES
    OUTPUT_NAME "dxcompiler"
    PREFIX ""
    SUFFIX ".dll"
)
