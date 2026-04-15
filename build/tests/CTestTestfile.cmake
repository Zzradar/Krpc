# CMake generated Testfile for 
# Source directory: /home/zzz/Krpc/tests
# Build directory: /home/zzz/Krpc/build/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(krpc_unit_tests "/home/zzz/Krpc/build/tests/krpc_unit_tests")
set_tests_properties(krpc_unit_tests PROPERTIES  _BACKTRACE_TRIPLES "/home/zzz/Krpc/tests/CMakeLists.txt;25;add_test;/home/zzz/Krpc/tests/CMakeLists.txt;0;")
subdirs("../_deps/catch2-build")
