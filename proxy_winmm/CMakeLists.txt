# ck3accel_proxy_winmm: the winmm.dll proxy (no-rename injection vehicle); rationale in
# src/dllmain.cpp. The .asm/.def/.inc are generated from the real winmm export table.
add_library(ck3accel_proxy_winmm SHARED
    src/dllmain.cpp
    src/winmm_thunks.asm
)
target_link_options(ck3accel_proxy_winmm PRIVATE
    /DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/winmm_forwards.def
)
set_target_properties(ck3accel_proxy_winmm PROPERTIES
    OUTPUT_NAME "winmm"
    PREFIX ""
    SUFFIX ".dll"
)
