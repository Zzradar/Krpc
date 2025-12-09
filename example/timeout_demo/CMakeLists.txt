file(GLOB TIMEOUT_CLIENT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../*.pb.cc)
add_executable(timeout_client ${TIMEOUT_CLIENT_SRCS} ${PROTO_SRCS})
target_link_libraries(timeout_client krpc_core ${LIBS})
target_compile_options(timeout_client PRIVATE -std=c++11 -Wall)
set_target_properties(timeout_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
