CMAKE_MINIMUM_REQUIRED(VERSION 3.13)

project(symdb)

set(PROJ_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR})
set (CMAKE_CXX_STANDARD 20)

option(USE_SYSTEM_PROTOBUF "Use pre-installed protobuf" OFF)
option(USE_SYSTEM_BOOST "Use pre-installed boost" OFF)

if ("${CLANG_ROOT}" STREQUAL "")
    find_package(Clang REQUIRED)
    message(STATUS "Clang include dir: ${CLANG_INCLUDE_DIRS}")
    include_directories(${CLANG_INCLUDE_DIRS})
else()
    include_directories(${CLANG_ROOT}/include)
endif()

cmake_policy(SET CMP0077 NEW)

if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set (CMAKE_INSTALL_PREFIX "$ENV{HOME}/.local/" CACHE PATH "default install path" FORCE )
endif()

message (STATUS "Innstall prefix: ${CMAKE_INSTALL_PREFIX}")

include(cmake/protobuf.cmake)
include(cmake/snappy.cmake)
include(cmake/leveldb.cmake)
include(cmake/pugixml.cmake)

include_directories(${PROTOBUF_INCLUDE_DIR})

add_subdirectory(proto)
add_subdirectory(src)
