set(SERVER_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/MsgpackServer.cc)
set(CLIENT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/MsgpackClient.cc)

add_executable(msgpack_server ${SERVER_SRCS})
add_executable(msgpack_client ${CLIENT_SRCS})

target_link_libraries(msgpack_server krpc_core ${LIBS})
target_link_libraries(msgpack_client krpc_core ${LIBS})

target_compile_options(msgpack_server PRIVATE -std=c++11 -Wall)
target_compile_options(msgpack_client PRIVATE -std=c++11 -Wall)

set_target_properties(msgpack_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
set_target_properties(msgpack_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)

