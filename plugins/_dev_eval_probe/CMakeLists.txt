add_library(accel_eval_probe SHARED
    src/plugin.cpp
)

target_include_directories(accel_eval_probe
    PRIVATE ${CMAKE_SOURCE_DIR}/core/include
)

set_target_properties(accel_eval_probe PROPERTIES
    OUTPUT_NAME "accel_eval_probe"
    PREFIX ""
    SUFFIX ".dll"
)

target_link_libraries(accel_eval_probe PRIVATE winmm)
