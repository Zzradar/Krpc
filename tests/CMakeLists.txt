# Unit tests: Protobuf + small helpers only (no Muduo / ZK), for CI and fast feedback.
include(FetchContent)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

add_executable(krpc_unit_tests
  test_header.cc
  test_frame_codec.cc
  test_controller.cc
  ${CMAKE_SOURCE_DIR}/src/Krpcheader.pb.cc
  ${CMAKE_SOURCE_DIR}/src/Krpccontroller.cc
)
target_include_directories(krpc_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${CMAKE_SOURCE_DIR}/src/include
)
target_link_libraries(krpc_unit_tests PRIVATE Catch2::Catch2WithMain protobuf)
target_compile_features(krpc_unit_tests PRIVATE cxx_std_11)

include(CTest)
add_test(NAME krpc_unit_tests COMMAND krpc_unit_tests)
