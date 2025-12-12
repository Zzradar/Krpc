# 获取 demo 源文件
file(GLOB ASYNC_DEMO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)

# 获取 protobuf 生成的 .cc
file(GLOB PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../*.pb.cc)

add_executable(async_client ${ASYNC_DEMO_SRCS} ${PROTO_SRCS})
target_link_libraries(async_client krpc_core ${LIBS})
target_compile_options(async_client PRIVATE -std=c++11 -Wall)

set_target_properties(async_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
