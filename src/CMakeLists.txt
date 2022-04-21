CMAKE_MINIMUM_REQUIRED(VERSION 3.1)

set( CMAKE_EXPORT_COMPILE_COMMANDS 1 )

set( CMAKE_CXX_STANDARD 14 )

function(add_symbol_executable target)
    file(GLOB_RECURSE SRC_FILES RELATIVE
                      ${CMAKE_CURRENT_SOURCE_DIR}
                      ${CMAKE_CURRENT_LIST_DIR}/*.cpp)

    add_executable( ${target} ${SRC_FILES} )
    set(extra_libs ${ARGN})

    TARGET_LINK_LIBRARIES(${target} proto symutil ${BOOST_LIBRARIES}
        ${PROTOBUF_LIBRARIES} pthread ${extra_libs})
endfunction(add_symbol_executable)

if (NOT USE_SYSTEM_BOOST)
    add_subdirectory(boost_parts)
else()
    set(Boost_USE_STATIC_LIBS ON)
    find_package(Boost COMPONENTS system log filesystem)
endif()

message (STATUS ${CMAKE_INSTALL_PREFIX})
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${PROJ_BUILD_DIR})
include_directories(${BOOST_INCLUDE_DIRS})
include_directories(${PUGIXML_INCLUDE_DIR})

add_subdirectory(util)
add_subdirectory(server)
add_subdirectory(client)
