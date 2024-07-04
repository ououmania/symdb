CMAKE_MINIMUM_REQUIRED(VERSION 3.13)

project(symdb)

set(PROJ_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR})

option(USE_SYSTEM_PROTOBUF "Use pre-installed protobuf" OFF)
option(USE_SYSTEM_BOOST "Use pre-installed boost" OFF)

# boost log also has a lot of dependencies
option(USE_BOOST_LOG "Use boost-log but it fails after c++17" OFF)

set (USE_BOOST_FILESYSTEM OFF)

include(CheckCXXCompilerFlag)
CHECK_CXX_COMPILER_FLAG("-std=c++20" COMPILER_SUPPORTS_CXX20)
if(COMPILER_SUPPORTS_CXX20)
    set (CMAKE_CXX_STANDARD 20)
else()
    CHECK_CXX_COMPILER_FLAG("-std=c++14" COMPILER_SUPPORTS_CXX14)
    if(COMPILER_SUPPORTS_CXX14)
        set (USE_BOOST_FILESYSTEM ON)
        set (CMAKE_CXX_STANDARD 14)
    else()
        message(FATAL_ERROR "A compiler with at least c++14 is required")
    endif()
endif()

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
