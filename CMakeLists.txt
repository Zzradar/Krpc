#设置最低版本和项目名称
cmake_minimum_required(VERSION 3.0)
project(Krpc)

#设置全局的C++标准
set(CMAKE_CXX_STANDARD 11)#在这里我设置了11，代表使用c++11标准
set(CMAKE_CXX_STANDARD_REQUIRED True)#CMAKE_CXX_STANDARD_REQUIRED表示强制使用指定的标准

#设置头文件目录，供所有子项目使用
#include_directories(${CMAKE_SOURCE_DIR}/src/include) #其中CMAKE_SOURCE_DIR表示当前目录

#查找porotbuf包
find_package(Protobuf REQUIRED)
include_directories(${Protobuf_INCLUDE_DIRS})#Protobuf_INCLUDE_DIRS表示protobuf头文件目录

#设置全局链接库
set(LIBS
    protobuf
    pthread
    zookeeper_mt
    muduo_net
    muduo_base
    glog
)
# 添加子目录
# 为了让 CI/本地单测在不依赖 Muduo/ZK 的情况下也能跑通：
# - KRPC_BUILD_FRAMEWORK=ON（默认）：构建完整框架 + example
# - KRPC_BUILD_FRAMEWORK=OFF：仅构建 tests（Catch2 单测），跳过 muduo/zookeeper 依赖
option(KRPC_BUILD_FRAMEWORK "Build muduo/zookeeper based framework and examples" ON)
if(KRPC_BUILD_FRAMEWORK)
    add_subdirectory(src)
    add_subdirectory(example)
endif()

option(KRPC_BUILD_TESTS "Build unit tests (Catch2, no Muduo)" ON)
if(KRPC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
