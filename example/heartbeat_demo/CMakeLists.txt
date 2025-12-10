file(GLOB HEARTBEAT_CLIENT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../*.pb.cc)
add_executable(heartbeat_client ${HEARTBEAT_CLIENT_SRCS} ${PROTO_SRCS})
target_link_libraries(heartbeat_client krpc_core ${LIBS})
target_compile_options(heartbeat_client PRIVATE -std=c++11 -Wall)
set_target_properties(heartbeat_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
