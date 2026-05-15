# CMakeList.txt : CMake project for CynLr, include source and define
# project specific logic here.

# ---------------------------------------------------------------------------
# Source lists — shared across all executables that need the full pipeline.
# Adding a new stage: put its .cpp in CYNLR_SOURCES and its .hpp in
# CYNLR_HEADERS.  Every target that links CYNLR_SOURCES picks it up
# automatically.
# ---------------------------------------------------------------------------

set(CYNLR_SOURCES
    ConfigManager.cpp
    GeneratorBlock.cpp
    Filterblock.cpp
    Labellingblock.cpp
    TracingBlock.cpp        # Phase 4 — Tracing & Computation stage
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
    TracingUtils.hpp        # Phase 4 — BlobAccumulator, CompletedBlob, ITracingOutput
    TracingBlock.hpp        # Phase 4 — TracingBlock class
)

# ---------------------------------------------------------------------------
# compiler_options interface target — inherited by all executables
# Defined in the top-level CMakeLists.txt via add_library(cynlr_compiler_options)
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# CynLr_multiThread — four-stage threaded pipeline
#   Generator → Filter → Labelling → Tracing
# ---------------------------------------------------------------------------
add_executable(CynLr_multiThread
    main.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}   # Listed so VS Solution Explorer shows them
)

target_include_directories(CynLr_multiThread
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(CynLr_multiThread
    PRIVATE
        cynlr_compiler_options
)

if(WIN32)
    target_link_libraries(CynLr_multiThread PRIVATE winmm)
endif()

set_property(TARGET CynLr_multiThread PROPERTY CXX_STANDARD          17)
set_property(TARGET CynLr_multiThread PROPERTY CXX_STANDARD_REQUIRED  ON)
set_property(TARGET CynLr_multiThread PROPERTY CXX_EXTENSIONS         OFF)

set_target_properties(CynLr_multiThread PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# CynLr_Linear — single-threaded baseline pipeline (measurement / profiling)
#   Inline generate → filter → label → trace (no queues, no threads)
# ---------------------------------------------------------------------------
add_executable(CynLr_Linear
    main_linear.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

target_include_directories(CynLr_Linear
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(CynLr_Linear
    PRIVATE cynlr_compiler_options
)

if(WIN32)
    target_link_libraries(CynLr_Linear PRIVATE winmm)
endif()

set_property(TARGET CynLr_Linear PROPERTY CXX_STANDARD          17)
set_property(TARGET CynLr_Linear PROPERTY CXX_STANDARD_REQUIRED  ON)
set_property(TARGET CynLr_Linear PROPERTY CXX_EXTENSIONS         OFF)

set_target_properties(CynLr_Linear PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# test_generator — unit tests for GeneratorBlock, DataSources, Queue, Config
# ---------------------------------------------------------------------------
add_executable(test_generator
    TestGenerator.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

target_include_directories(test_generator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_generator PRIVATE cynlr_compiler_options)

if(WIN32)
    target_link_libraries(test_generator PRIVATE winmm)
endif()

set_property(TARGET test_generator PROPERTY CXX_STANDARD          17)
set_property(TARGET test_generator PROPERTY CXX_STANDARD_REQUIRED  ON)
set_property(TARGET test_generator PROPERTY CXX_EXTENSIONS         OFF)

set_target_properties(test_generator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# test_filter — unit tests for FilterBlock, SlidingWindow, BinaryThresholder
# ---------------------------------------------------------------------------
add_executable(test_filter
    TestFilter.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

target_include_directories(test_filter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_filter PRIVATE cynlr_compiler_options)

if(WIN32)
    target_link_libraries(test_filter PRIVATE winmm)
endif()

set_property(TARGET test_filter PROPERTY CXX_STANDARD          17)
set_property(TARGET test_filter PROPERTY CXX_STANDARD_REQUIRED  ON)
set_property(TARGET test_filter PROPERTY CXX_EXTENSIONS         OFF)

set_target_properties(test_filter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# test_labelling — unit tests for LabellingBlock, LabelMap, RowLabelBuffer
# ---------------------------------------------------------------------------
add_executable(test_labelling
    TestLabelling.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

target_include_directories(test_labelling PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_labelling PRIVATE cynlr_compiler_options)

if(WIN32)
    target_link_libraries(test_labelling PRIVATE winmm)
endif()

set_property(TARGET test_labelling PROPERTY CXX_STANDARD          17)
set_property(TARGET test_labelling PROPERTY CXX_STANDARD_REQUIRED  ON)
set_property(TARGET test_labelling PROPERTY CXX_EXTENSIONS         OFF)

set_target_properties(test_labelling PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# test_tracing — unit tests for TracingBlock, BlobAccumulator, CompletedBlob
# Phase 4 addition.
# ---------------------------------------------------------------------------
add_executable(test_tracing
    TestTracing.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

target_include_directories(test_tracing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_tracing PRIVATE cynlr_compiler_options)

if(WIN32)
    target_link_libraries(test_tracing PRIVATE winmm)
endif()

set_property(TARGET test_tracing PROPERTY CXX_STANDARD          17)
set_property(TARGET test_tracing PROPERTY CXX_STANDARD_REQUIRED  ON)
set_property(TARGET test_tracing PROPERTY CXX_EXTENSIONS         OFF)

set_target_properties(test_tracing PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---------------------------------------------------------------------------
# CTest integration
# Runs all four test suites via: cmake --build build --target RUN_TESTS
# or:                            ctest --test-dir build
# Each suite is registered individually so a single failing suite does not
# mask failures in the others.
# ---------------------------------------------------------------------------
enable_testing()

if(CYNLR_USE_GTEST)
    # GoogleTest path — links gtest_main and uses gtest_discover_tests.
    foreach(_tgt test_generator test_filter test_labelling test_tracing)
        target_link_libraries(${_tgt} PRIVATE gtest_main)
        include(GoogleTest)
        gtest_discover_tests(${_tgt})
    endforeach()
else()
    # Lightweight built-in framework path — detect [FAIL] in stdout.
    add_test(NAME GeneratorBlock_Tests  COMMAND test_generator)
    add_test(NAME FilterBlock_Tests     COMMAND test_filter)
    add_test(NAME LabellingBlock_Tests  COMMAND test_labelling)
    add_test(NAME TracingBlock_Tests    COMMAND test_tracing)   # Phase 4

    foreach(_name GeneratorBlock_Tests FilterBlock_Tests
                  LabellingBlock_Tests TracingBlock_Tests)
        set_tests_properties(${_name} PROPERTIES
            FAIL_REGULAR_EXPRESSION "\\[FAIL\\]"
        )
    endforeach()
endif()
