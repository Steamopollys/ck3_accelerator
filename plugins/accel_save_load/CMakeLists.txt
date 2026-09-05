# accel_save_load: shipping plugin. Swaps CK3's gamestate decompression (raw
# DEFLATE) for libdeflate over a buffered input, CRC-verified before writing.
# Links libdeflate statically, nothing from core (C ABI in <ck3accel/core_api.h>).
add_library(accel_save_load SHARED
    src/plugin.cpp
)

target_include_directories(accel_save_load
    PRIVATE ${CMAKE_SOURCE_DIR}/core/include
)

# libdeflate_static carries its own PUBLIC include dir for <libdeflate.h>.
target_link_libraries(accel_save_load
    PRIVATE libdeflate::libdeflate_static
)

set_target_properties(accel_save_load PROPERTIES
    OUTPUT_NAME "accel_save_load"
    PREFIX ""
    SUFFIX ".dll"
)
