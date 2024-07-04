# Necessary for symdb to analyze our project.
set( CMAKE_EXPORT_COMPILE_COMMANDS 1 )

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

message (STATUS "installed to ${CMAKE_INSTALL_PREFIX}")
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
include_directories(${PROJ_BUILD_DIR})
include_directories(${BOOST_INCLUDE_DIRS})
include_directories(${PUGIXML_INCLUDE_DIR})
include_directories(${PROJECT_SOURCE_DIR}/third_party/spdlog/include)

if (CMAKE_COMPILER_IS_GNUCC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 8.0)
    string(APPEND CMAKE_CXX_FLAGS " -fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/=''")
else()
    # 移除目标文件的.o后缀
    set(FILEPATH "$(patsubst %.o,%,$(abspath $@))")# $@ is the name of the target being generated
    # 移除其中的输出目录
    set(FILEPATH "$(subst ${WX_SRC_DIR},,${FILEPATH})")
    set(FILEPATH "$(subst ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${TARGET}.dir/,,${FILEPATH})")
    string(APPEND CMAKE_CXX_FLAGS " -Wno-builtin-macro-redefined")
    string(APPEND CMAKE_CXX_FLAGS " -U__FILE__ -D__FILE__='\"${FILEPATH}\"'")
endif()

add_subdirectory(util)
add_subdirectory(server)
add_subdirectory(client)
