# accel_tick_cache: opt-in. Whitelisted pure-trigger result cache during the tick.
add_library(accel_tick_cache SHARED src/plugin.cpp)
target_include_directories(accel_tick_cache PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(accel_tick_cache PRIVATE psapi)
set_target_properties(accel_tick_cache PROPERTIES OUTPUT_NAME "accel_tick_cache" PREFIX "" SUFFIX ".dll")
