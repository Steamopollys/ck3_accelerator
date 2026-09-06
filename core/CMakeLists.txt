# core object lib (DLL + tests)
add_library(ck3accel_core_objs OBJECT
    src/paths.cpp
    src/logger.cpp
    src/config.cpp
    src/pe_inspect.cpp
    src/versions_json.cpp
    src/version_detect.cpp
    src/core_init.cpp
    src/pattern_scanner.cpp
    src/hook_engine.cpp
    src/plugin_gate.cpp
    src/telemetry.cpp
    src/kill_switch.cpp
    src/crash_sentinel.cpp
    src/plugin_loader.cpp
    src/tick_epoch.cpp
    src/panel_registry.cpp
    src/trigger_service.cpp
)

target_include_directories(ck3accel_core_objs
    PUBLIC  include
    PRIVATE src
)

target_link_libraries(ck3accel_core_objs
    PRIVATE
        spdlog::spdlog
        tomlplusplus::tomlplusplus
        picosha2
        minhook
        psapi
)

# Main DLL target
add_library(ck3accel_core SHARED
    src/dllmain.cpp
    $<TARGET_OBJECTS:ck3accel_core_objs>
)

target_include_directories(ck3accel_core
    PUBLIC  include
    PRIVATE src
)

target_link_libraries(ck3accel_core
    PRIVATE
        spdlog::spdlog
        tomlplusplus::tomlplusplus
        picosha2
        minhook
        psapi
)

set_target_properties(ck3accel_core PROPERTIES
    OUTPUT_NAME "ck3accel_core"
    PREFIX ""
    SUFFIX ".dll"
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

# dev utility: fills in versions.json entries when a new CK3 build appears.
# Not shipped.
add_executable(ck3accel_fingerprint_tool src/_dev_fingerprint_tool.cpp)
target_link_libraries(ck3accel_fingerprint_tool PRIVATE picosha2)
