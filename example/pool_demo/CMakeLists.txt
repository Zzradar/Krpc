file(GLOB POOL_DEMO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cc)
file(GLOB PROTO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/../*.pb.cc)

add_executable(pool_demo ${POOL_DEMO_SRCS} ${PROTO_SRCS})
target_link_libraries(pool_demo krpc_core ${LIBS})
target_compile_options(pool_demo PRIVATE -std=c++11 -Wall)

set_target_properties(pool_demo PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
