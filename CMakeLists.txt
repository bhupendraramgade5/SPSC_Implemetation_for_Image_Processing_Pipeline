# CMakeList.txt : Top-level CMake project file, do global configuration
# and include sub-projects here.
#
# cmake_minimum_required (VERSION 3.10)
cmake_minimum_required(VERSION 3.14)

# Enable Hot Reload for MSVC compilers if supported.

if(POLICY CMP0141)
    cmake_policy(SET CMP0141 NEW)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
        "$<IF:$<AND:$<C_COMPILER_ID:MSVC>,$<CXX_COMPILER_ID:MSVC>>,\
$<$<CONFIG:Debug,RelWithDebInfo>:EditAndContinue>,\
$<$<CONFIG:Debug,RelWithDebInfo>:ProgramDatabase>>")
endif()

project(CynLr
    VERSION     1.0.0
    DESCRIPTION "CynLr line-scan pipeline evaluation"
    LANGUAGES   CXX
)

add_library(cynlr_compiler_options INTERFACE)
target_compile_features(cynlr_compiler_options INTERFACE cxx_std_17)

# project ("CynLr")
if(MSVC)
    target_compile_options(cynlr_compiler_options INTERFACE
        /W4         # Warning level 4
        /WX         # Treat warnings as errors
        /permissive-    # Strict conformance mode
        /Zc:__cplusplus # Report correct __cplusplus value (MSVC quirk)
    )
else()
    # GCC / Clang  (useful when building on Linux CI or WSL)
    target_compile_options(cynlr_compiler_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
    )
endif()


# Usage from command line:
#   cmake -S . -B build -DCYNLR_USE_GTEST=ON
option(CYNLR_PERF "Enable performance instrumentation" OFF)
if(CYNLR_PERF)
    target_compile_definitions(cynlr_compiler_options INTERFACE CYNLR_PERF_BUILD)
    message(STATUS "Performance instrumentation ENABLED")
else()
    message(STATUS "Performance instrumentation DISABLED")
endif()


if(CYNLR_USE_GTEST)
    include(FetchContent)
 
    fetchcontent_declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.14.0     # pinned tag — never use 'main' in builds
    )
 
    # Prevent GTest from overriding our compiler/linker settings on Windows.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
 
    # Do not install GTest alongside the project's own install targets.
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
 
    fetchcontent_makeavailable(googletest)
 
    message(STATUS "GoogleTest enabled (v1.14.0 via FetchContent)")
else()
    message(STATUS "GoogleTest disabled — using built-in test framework")
    message(STATUS "  To enable: cmake -DCYNLR_USE_GTEST=ON ...")
endif()

# Include sub-projects.
add_subdirectory ("src")
