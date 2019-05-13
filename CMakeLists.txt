CMAKE_MINIMUM_REQUIRED(VERSION 3.13)

project(symdb)

set(PROJ_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR})
message(STATUS ${PROJ_BUILD_DIR})

option(USE_SYSTEM_PROTOBUF "Use pre-installed protobuf" OFF)
option(USE_SYSTEM_BOOST "Use pre-installed boost" OFF)

cmake_policy(SET CMP0077 NEW)

include(cmake/protobuf.cmake)
include(cmake/snappy.cmake)
include(cmake/leveldb.cmake)

include_directories(${PROTOBUF_INCLUDE_DIR})

add_subdirectory(proto)
add_subdirectory(src)
