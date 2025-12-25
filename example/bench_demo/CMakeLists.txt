file(GLOB BENCH_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../*.pb.cc)

add_executable(bench_demo ${BENCH_SRCS} ${PROTO_SRCS})
target_link_libraries(bench_demo krpc_core ${LIBS})
target_compile_options(bench_demo PRIVATE -std=c++11 -Wall)

set_target_properties(bench_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
