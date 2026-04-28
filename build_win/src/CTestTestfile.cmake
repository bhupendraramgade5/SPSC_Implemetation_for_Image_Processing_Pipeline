# CMake generated Testfile for 
# Source directory: C:/Users/bhupe/Desktop/CynLr/src
# Build directory: C:/Users/bhupe/Desktop/CynLr/build_win/src
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")
  add_test(GeneratorBlock_Tests "C:/Users/bhupe/Desktop/CynLr/build_win/bin/Debug/test_generator.exe")
  set_tests_properties(GeneratorBlock_Tests PROPERTIES  FAIL_REGULAR_EXPRESSION "\\[FAIL\\]" _BACKTRACE_TRIPLES "C:/Users/bhupe/Desktop/CynLr/src/CMakeLists.txt;96;add_test;C:/Users/bhupe/Desktop/CynLr/src/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ee][Aa][Ss][Ee])$")
  add_test(GeneratorBlock_Tests "C:/Users/bhupe/Desktop/CynLr/build_win/bin/Release/test_generator.exe")
  set_tests_properties(GeneratorBlock_Tests PROPERTIES  FAIL_REGULAR_EXPRESSION "\\[FAIL\\]" _BACKTRACE_TRIPLES "C:/Users/bhupe/Desktop/CynLr/src/CMakeLists.txt;96;add_test;C:/Users/bhupe/Desktop/CynLr/src/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Mm][Ii][Nn][Ss][Ii][Zz][Ee][Rr][Ee][Ll])$")
  add_test(GeneratorBlock_Tests "C:/Users/bhupe/Desktop/CynLr/build_win/bin/MinSizeRel/test_generator.exe")
  set_tests_properties(GeneratorBlock_Tests PROPERTIES  FAIL_REGULAR_EXPRESSION "\\[FAIL\\]" _BACKTRACE_TRIPLES "C:/Users/bhupe/Desktop/CynLr/src/CMakeLists.txt;96;add_test;C:/Users/bhupe/Desktop/CynLr/src/CMakeLists.txt;0;")
elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll][Ww][Ii][Tt][Hh][Dd][Ee][Bb][Ii][Nn][Ff][Oo])$")
  add_test(GeneratorBlock_Tests "C:/Users/bhupe/Desktop/CynLr/build_win/bin/RelWithDebInfo/test_generator.exe")
  set_tests_properties(GeneratorBlock_Tests PROPERTIES  FAIL_REGULAR_EXPRESSION "\\[FAIL\\]" _BACKTRACE_TRIPLES "C:/Users/bhupe/Desktop/CynLr/src/CMakeLists.txt;96;add_test;C:/Users/bhupe/Desktop/CynLr/src/CMakeLists.txt;0;")
else()
  add_test(GeneratorBlock_Tests NOT_AVAILABLE)
endif()
