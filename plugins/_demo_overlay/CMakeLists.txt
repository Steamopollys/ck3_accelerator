# accel_demo_overlay: DEMONSTRATION plugin (opt-in, not shipped). Dear ImGui overlay drawn over the
# game via a D3D11 Present hook. Depends on the imgui submodule.

# Dear ImGui + its DX11/Win32 backends, built with warnings off (vendored, not held to /W4 /WX).
set(IMGUI ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui_dx11 STATIC
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/backends/imgui_impl_dx11.cpp
    ${IMGUI}/backends/imgui_impl_win32.cpp
)
target_include_directories(imgui_dx11 PUBLIC ${IMGUI} ${IMGUI}/backends)
target_compile_options(imgui_dx11 PRIVATE /W0)

add_library(accel_demo_overlay SHARED src/plugin.cpp)
target_include_directories(accel_demo_overlay PRIVATE ${CMAKE_SOURCE_DIR}/core/include ${IMGUI})
target_link_libraries(accel_demo_overlay PRIVATE imgui_dx11 d3d11 dxgi d3dcompiler psapi)
set_target_properties(accel_demo_overlay PROPERTIES OUTPUT_NAME "accel_demo_overlay" PREFIX "" SUFFIX ".dll")
