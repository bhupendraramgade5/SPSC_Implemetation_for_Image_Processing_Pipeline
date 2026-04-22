# CMakeList.txt : CMake project for CynLr, include source and define
# project specific logic here.
#

# Add source to this project's executable.
# add_executable (CynLr "CynLr.cpp" )

add_executable(CynLr
    main.cpp
    ConfigManager.cpp
    GeneratorBlock.cpp
)

if (CMAKE_VERSION VERSION_GREATER 3.12)
  set_property(TARGET CynLr PROPERTY CXX_STANDARD 20)
endif()

# TODO: Add tests and install targets if needed.
