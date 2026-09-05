add_library(accel_tick_cache_probe SHARED src/plugin.cpp)
target_include_directories(accel_tick_cache_probe PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(accel_tick_cache_probe PRIVATE psapi)
set_target_properties(accel_tick_cache_probe PROPERTIES OUTPUT_NAME "accel_tick_cache_probe" PREFIX "" SUFFIX ".dll")
