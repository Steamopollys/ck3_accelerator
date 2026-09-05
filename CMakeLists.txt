cmake_minimum_required(VERSION 3.25)
project(ck3accel
    VERSION 0.0.1
    DESCRIPTION "CK3 Accelerator: runtime engine accelerator for Crusader Kings III"
    LANGUAGES CXX C
)

# Enforce out-of-source builds
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not allowed. Create a separate build directory.")
endif()

# C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC only; bail on other compilers
if(NOT MSVC)
    message(FATAL_ERROR "ck3accel requires MSVC. Detected: ${CMAKE_CXX_COMPILER_ID}")
endif()

# MSVC flags
add_compile_options(
    /W4              # high warning level
    /WX              # warnings as errors
    /permissive-     # strict conformance
    /Zc:__cplusplus  # report correct __cplusplus value
    /utf-8           # source + execution charset
    /EHsc            # standard C++ exceptions
    /MP              # parallel compile
)
add_compile_definitions(
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

# Submodule dependencies
add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)
add_subdirectory(third_party/spdlog EXCLUDE_FROM_ALL)
add_subdirectory(third_party/tomlplusplus EXCLUDE_FROM_ALL)
add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)

# libdeflate: static only. Keep the default zlib/gzip format decoders (header
# flags); disable the gzip CLI and shared lib. Linked ONLY into accel_save_load,
# never core.
set(LIBDEFLATE_BUILD_SHARED_LIB OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_STATIC_LIB ON  CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_GZIP       OFF CACHE BOOL "" FORCE)
set(LIBDEFLATE_BUILD_TESTS      OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/libdeflate EXCLUDE_FROM_ALL)

# vendored C deps aren't built under our /W4 /WX; silence warnings on those
# targets only, never our code.
if(TARGET minhook)
    target_compile_options(minhook PRIVATE /W0)
endif()
if(TARGET libdeflate_static)
    target_compile_options(libdeflate_static PRIVATE /W0)
endif()

# picosha2 is header-only; expose as INTERFACE lib
add_library(picosha2 INTERFACE)
target_include_directories(picosha2 INTERFACE third_party/picosha2)

# Project components
enable_testing()
add_subdirectory(core)
add_subdirectory(proxy_dxc)
add_subdirectory(plugins)
