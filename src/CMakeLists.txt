# CMakeList.txt : CMake project for CynLr, include source and define
# project specific logic here.
#

# Add source to this project's executable.
# add_executable (CynLr "CynLr.cpp" )

# Added new 
set(CYNLR_SOURCES
    ConfigManager.cpp
    GeneratorBlock.cpp
    Filterblock.cpp
    # FilterBlock.cpp     <- uncomment when implemented
)
# added new 
set(CYNLR_HEADERS
    ConfigManager.hpp
    FilterUtils.hpp
    Filterblock.hpp
    GeneratorBlock.hpp
    Queue.hpp
    # FilterBlock.hpp     <- uncomment when implemented
)

# Commented for more flexible source/header management, and to avoid listing headers in the executable target.
# add_executable(CynLr
#     main.cpp
#     ConfigManager.cpp
#     GeneratorBlock.cpp
# )

add_executable(CynLr
    main.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}   # Listed so VS Solution Explorer shows them in the IDE
)

target_include_directories(CynLr
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(CynLr
    PRIVATE
        cynlr_compiler_options
)


if(WIN32)
    target_link_libraries(CynLr PRIVATE winmm)
endif()

set_property(TARGET CynLr PROPERTY CXX_STANDARD          17)
set_property(TARGET CynLr PROPERTY CXX_STANDARD_REQUIRED  ON)
set_property(TARGET CynLr PROPERTY CXX_EXTENSIONS         OFF)

set_target_properties(CynLr PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Test executable
add_executable(test_generator
    TestGenerator.cpp
    ${CYNLR_SOURCES}
    ${CYNLR_HEADERS}
)

# TODO: Add tests and install targets if needed.
target_include_directories(test_generator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_generator PRIVATE cynlr_compiler_options)

# Register with CTest so "cmake --build . --target RUN_TESTS" works from VS.
if(WIN32)
    target_link_libraries(test_generator PRIVATE winmm)
endif()

set_property(TARGET test_generator PROPERTY CXX_STANDARD          17)
set_property(TARGET test_generator PROPERTY CXX_STANDARD_REQUIRED  ON)
set_property(TARGET test_generator PROPERTY CXX_EXTENSIONS         OFF)

set_target_properties(test_generator PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

enable_testing()
# add_test(NAME GeneratorBlockTests COMMAND test_generator)

if(CYNLR_USE_GTEST)
    target_link_libraries(test_generator PRIVATE gtest_main)

    include(GoogleTest)
    gtest_discover_tests(test_generator)
else()
    add_test(
      NAME    GeneratorBlock_Tests
      COMMAND test_generator
    )

    set_tests_properties(GeneratorBlock_Tests PROPERTIES
      FAIL_REGULAR_EXPRESSION "\\[FAIL\\]"
    )
endif()
