CMAKE_MINIMUM_REQUIRED(VERSION 3.7)

project(symdb)

find_package(Protobuf REQUIRED)

set(PROJ_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR})
message(STATUS ${PROJ_BUILD_DIR})

add_subdirectory(proto)
add_subdirectory(src)
