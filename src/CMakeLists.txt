CMAKE_MINIMUM_REQUIRED(VERSION 3.7)

set (CMAKE_CXX_STANDARD 17)

set( CMAKE_EXPORT_COMPILE_COMMANDS 1 )

include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${PROJ_BUILD_DIR})

function(add_symbol_executable target)
    file(GLOB_RECURSE SRC_FILES RELATIVE
                      ${CMAKE_CURRENT_SOURCE_DIR}
                      ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

    add_executable( ${target} ${SRC_FILES} )
    set(extra_libs ${ARGN})

    set(Boost_USE_STATIC_LIBS ON)
    find_package(Boost COMPONENTS system log filesystem program_options)

    find_library(Clang_LIBRARY NAMES clang)
    set(Protobuf_USE_STATIC_LIB ON)
    TARGET_LINK_LIBRARIES(${target} proto symutil ${Boost_LIBRARIES}
        ${Protobuf_LIBRARIES} ${Clang_LIBRARY} pthread ${extra_libs})
endfunction(add_symbol_executable)

add_subdirectory(util)
add_subdirectory(server)
add_subdirectory(client)
