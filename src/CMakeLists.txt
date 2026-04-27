# CMakeList.txt : CMake project for CynLr, include source and define
# project specific logic here.
#

# Add source to this project's executable.
# add_executable (CynLr "CynLr.cpp" )

# Added new 
set(CYNLR_SOURCES
    main.cpp
    ConfigManager.cpp
    GeneratorBlock.cpp
    # FilterBlock.cpp     <- uncomment when implemented
)
# added new 
set(CYNLR_HEADERS
    ConfigManager.hpp
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


# TODO: Add tests and install targets if needed.

# Register with CTest so "cmake --build . --target RUN_TESTS" works from VS.
enable_testing()
add_test(NAME GeneratorBlockTests COMMAND test_generator)