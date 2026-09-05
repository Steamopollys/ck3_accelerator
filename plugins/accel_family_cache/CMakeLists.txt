# accel_family_cache: opt-in. Per-frame memoization of the composite family-list
# builders (character-window portrait scans). Links nothing from core; uses the
# header-only, unit-tested <ck3accel/family_list_cache.h>.
add_library(accel_family_cache SHARED
    src/plugin.cpp
)
target_include_directories(accel_family_cache
    PRIVATE ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(accel_family_cache PRIVATE psapi)
set_target_properties(accel_family_cache PROPERTIES
    OUTPUT_NAME "accel_family_cache"
    PREFIX ""
    SUFFIX ".dll"
)
