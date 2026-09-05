add_library(accel_sampler SHARED
    src/plugin.cpp
)

target_include_directories(accel_sampler
    PRIVATE ${CMAKE_SOURCE_DIR}/core/include
)

set_target_properties(accel_sampler PROPERTIES
    OUTPUT_NAME "accel_sampler"
    PREFIX ""
    SUFFIX ".dll"
)

target_link_libraries(accel_sampler PRIVATE winmm)
