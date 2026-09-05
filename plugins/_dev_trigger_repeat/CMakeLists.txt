add_library(accel_trigger_repeat SHARED
    src/plugin.cpp
)
target_include_directories(accel_trigger_repeat
    PRIVATE ${CMAKE_SOURCE_DIR}/core/include
)
set_target_properties(accel_trigger_repeat PROPERTIES
    OUTPUT_NAME "accel_trigger_repeat"
    PREFIX ""
    SUFFIX ".dll"
)
