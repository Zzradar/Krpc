set(SWITCH_SERVER_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/SwitchServer.cc)
set(SWITCH_CLIENT_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/SwitchClient.cc)
file(GLOB PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../*.pb.cc)

add_executable(switch_server ${SWITCH_SERVER_SRCS} ${PROTO_SRCS})
add_executable(switch_client ${SWITCH_CLIENT_SRCS} ${PROTO_SRCS})

target_link_libraries(switch_server krpc_core ${LIBS})
target_link_libraries(switch_client krpc_core ${LIBS})

target_compile_options(switch_server PRIVATE -std=c++11 -Wall)
target_compile_options(switch_client PRIVATE -std=c++11 -Wall)

set_target_properties(switch_server PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
set_target_properties(switch_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
