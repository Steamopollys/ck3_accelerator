add_library(accel_attrib_probe SHARED
    src/plugin.cpp
)
target_include_directories(accel_attrib_probe
    PRIVATE ${CMAKE_SOURCE_DIR}/core/include
)
target_link_libraries(accel_attrib_probe PRIVATE psapi winmm)
set_target_properties(accel_attrib_probe PROPERTIES
    OUTPUT_NAME "accel_attrib_probe"
    PREFIX ""
    SUFFIX ".dll"
)
