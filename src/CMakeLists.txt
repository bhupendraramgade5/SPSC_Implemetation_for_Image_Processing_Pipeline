# ============================================================================
# Source lists — shared across all executables
# ============================================================================

set(CYNLR_SOURCES
    ConfigManager.cpp
    GeneratorBlock.cpp
    Filterblock.cpp
    Labellingblock.cpp
    TracingBlock.cpp
)

set(CYNLR_HEADERS
    ConfigManager.hpp
    FilterUtils.hpp
    Filterblock.hpp
    GeneratorBlock.hpp
    Queue.hpp
    PerfTest.hpp
    OutputWriter.hpp
    Labellingutils.hpp
    Labellingblock.hpp
    TracingUtils.hpp
    TracingBlock.hpp
)

# ============================================================================
# Optional performance instrumentation
# ----------------------------------------------------------------------------
# Enables:
#   - per-stage latency tracking
#   - timestamp fields in packets
#   - performance analysis blocks
#
# Usage:
#   cmake -DCYNLR_ENABLE_PERF=ON ..
# ============================================================================

option(CYNLR_ENABLE_PERF "Enable performance instrumentation" ON)

if(CYNLR_ENABLE_PERF)
    message(STATUS "CYNLR_PERF_BUILD ENABLED")
    add_compile_definitions(CYNLR_PERF_BUILD)
else()
    message(STATUS "CYNLR_PERF_BUILD DISABLED")
endif()

# ============================================================================
# Shared helper function for all executables
# ============================================================================

function(cynlr_configure_target target_name)

    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_link_libraries(${target_name}
        PRIVATE
            cynlr_compiler_options
    )

    if(WIN32)
        target_link_libraries(${target_name}
            PRIVATE
                winmm
        )
    endif()

    set_property(TARGET ${target_name} PROPERTY CXX_STANDARD 17)
    set_property(TARGET ${target_name} PROPERTY CXX_STANDARD_REQUIRED ON)
    set_property(TARGET ${target_name} PROPERTY CXX_EXTENSIONS OFF)

    set_target_properties(${target_name} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    )

endfunction()

# ============================================================================
# CynLr_multiThread
# ============================================================================

add_executable(CynLr_multiThread
    main.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

cynlr_configure_target(CynLr_multiThread)

# ============================================================================
# CynLr_Linear
# ============================================================================

add_executable(CynLr_Linear
    main_linear.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

cynlr_configure_target(CynLr_Linear)

# ============================================================================
# test_generator
# ============================================================================

add_executable(test_generator
    TestGenerator.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

cynlr_configure_target(test_generator)

# ============================================================================
# test_filter
# ============================================================================

add_executable(test_filter
    TestFilter.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

cynlr_configure_target(test_filter)

# ============================================================================
# test_labelling
# ============================================================================

add_executable(test_labelling
    TestLabelling.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

cynlr_configure_target(test_labelling)

# ============================================================================
# test_tracing
# ============================================================================

add_executable(test_tracing
    TestTracing.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

cynlr_configure_target(test_tracing)

# ============================================================================
# CTest integration
# ============================================================================

enable_testing()

if(CYNLR_USE_GTEST)

    foreach(_tgt
        test_generator
        test_filter
        test_labelling
        test_tracing)

        target_link_libraries(${_tgt}
            PRIVATE
                gtest_main
        )

        include(GoogleTest)
        gtest_discover_tests(${_tgt})

    endforeach()

else()

    add_test(NAME GeneratorBlock_Tests COMMAND test_generator)
    add_test(NAME FilterBlock_Tests COMMAND test_filter)
    add_test(NAME LabellingBlock_Tests COMMAND test_labelling)
    add_test(NAME TracingBlock_Tests COMMAND test_tracing)

    foreach(_name
        GeneratorBlock_Tests
        FilterBlock_Tests
        LabellingBlock_Tests
        TracingBlock_Tests)

        set_tests_properties(${_name} PROPERTIES
            FAIL_REGULAR_EXPRESSION "\\[FAIL\\]"
        )

    endforeach()

endif()