# accel_demo: DEMO plugin (opt-in, not shipped). Shows the framework changing gameplay
# by overriding an engine trigger's result. NOT checksum-neutral; breaks Ironman achievements.
add_library(accel_demo SHARED src/plugin.cpp)
target_include_directories(accel_demo PRIVATE ${CMAKE_SOURCE_DIR}/core/include)
target_link_libraries(accel_demo PRIVATE psapi)
set_target_properties(accel_demo PROPERTIES OUTPUT_NAME "accel_demo" PREFIX "" SUFFIX ".dll")
