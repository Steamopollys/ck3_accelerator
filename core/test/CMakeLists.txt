add_executable(ck3accel_core_tests
    test_paths.cpp
    test_config.cpp
    test_versions_json.cpp
    test_pattern_scanner.cpp
    test_hook_engine.cpp
    test_plugin_gate.cpp
    test_telemetry.cpp
    test_kill_switch.cpp
    test_crash_sentinel.cpp
    test_plugin_loader.cpp
    test_function_table.cpp
    test_sample_histogram.cpp
    test_probe_config.cpp
    test_eval_accumulators.cpp
    test_tsc_clock.cpp
    test_long_day_ring.cpp
    test_repeat_table.cpp
    test_family_dedup.cpp
    test_attrib_tables.cpp
    test_trigger_cache.cpp
    test_family_list_cache.cpp
)

target_link_libraries(ck3accel_core_tests
    PRIVATE
        ck3accel_core_objs
        GTest::gtest_main
        spdlog::spdlog
        tomlplusplus::tomlplusplus
        picosha2
        minhook
        psapi
)

target_include_directories(ck3accel_core_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src     # internal headers
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_compile_definitions(ck3accel_core_tests
    PRIVATE
        CK3ACCEL_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

include(GoogleTest)
gtest_discover_tests(ck3accel_core_tests)
