# accel_ui_trigger_cache: shipping plugin. Per-frame memoization of script trigger
# results during main-thread UI eval (character window / court positions / marriage
# window). Links nothing from core; uses header-only, unit-tested
# <ck3accel/trigger_cache.h>.
add_library(accel_ui_trigger_cache SHARED
    src/plugin.cpp
)
target_include_directories(accel_ui_trigger_cache
    PRIVATE ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(accel_ui_trigger_cache PRIVATE psapi)
set_target_properties(accel_ui_trigger_cache PROPERTIES
    OUTPUT_NAME "accel_ui_trigger_cache"
    PREFIX ""
    SUFFIX ".dll"
)
