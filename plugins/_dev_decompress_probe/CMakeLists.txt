add_library(accel_decompress_probe SHARED
    src/plugin.cpp
)

target_include_directories(accel_decompress_probe
    PRIVATE ${CMAKE_SOURCE_DIR}/core/include
)

set_target_properties(accel_decompress_probe PROPERTIES
    OUTPUT_NAME "accel_decompress_probe"
    PREFIX ""
    SUFFIX ".dll"
)
