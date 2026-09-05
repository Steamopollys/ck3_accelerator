# accel_family_lists: shipping plugin. Swaps the engine's O(N^2) family-list dedup
# (close/extended/close_or_extended_family_member builders) for an O(N) hash-set
# dedup with identical output. Links nothing from core (C ABI in
# <ck3accel/core_api.h>); algorithm is header-only <ck3accel/family_dedup.h>,
# tested in core/test/test_family_dedup.cpp.
add_library(accel_family_lists SHARED
    src/plugin.cpp
)

target_include_directories(accel_family_lists
    PRIVATE ${CMAKE_SOURCE_DIR}/core/include
)

set_target_properties(accel_family_lists PROPERTIES
    OUTPUT_NAME "accel_family_lists"
    PREFIX ""
    SUFFIX ".dll"
)
