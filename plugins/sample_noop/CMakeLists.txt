add_library(sample_noop SHARED
    src/plugin.cpp
)

target_include_directories(sample_noop
    PRIVATE ${CMAKE_SOURCE_DIR}/core/include
)

set_target_properties(sample_noop PROPERTIES
    OUTPUT_NAME "sample_noop"
    PREFIX ""
    SUFFIX ".dll"
)
