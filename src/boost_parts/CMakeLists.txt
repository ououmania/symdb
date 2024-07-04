CMAKE_MINIMUM_REQUIRED(VERSION 3.7)

set(BOOST_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)
set(LOCAL_BOOST_LIBRARIES)

macro(add_boost_lib target src_dir)
    file(GLOB_RECURSE SRC_FILES RELATIVE
                      ${CMAKE_CURRENT_SOURCE_DIR}
                      ${src_dir}/*.cpp)

    if (NOT WIN32)
        file(GLOB_RECURSE WIN_SRC_FILES RELATIVE
                          ${CMAKE_CURRENT_SOURCE_DIR}
                          ${src_dir}/windows/*.cpp)
        if (WIN_SRC_FILES)
            list(REMOVE_ITEM SRC_FILES ${WIN_SRC_FILES})
        endif()
        file(GLOB_RECURSE WIN_SRC_FILES RELATIVE
                          ${CMAKE_CURRENT_SOURCE_DIR}
                          ${src_dir}/win32/*.cpp)
        if (WIN_SRC_FILES)
            list(REMOVE_ITEM SRC_FILES ${WIN_SRC_FILES})
        endif()
    endif()
    add_library( ${target} STATIC ${SRC_FILES} )
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${src_dir})

    list(APPEND LOCAL_BOOST_LIBRARIES ${target})
endmacro(add_boost_lib)

add_boost_lib(boost_system ${CMAKE_CURRENT_SOURCE_DIR}/libs/system)
add_boost_lib(boost_regex ${CMAKE_CURRENT_SOURCE_DIR}/libs/regex)

if(USE_BOOST_LOG)
add_boost_lib(boost_filesystem ${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem)
add_boost_lib(boost_log ${CMAKE_CURRENT_SOURCE_DIR}/libs/log)
add_boost_lib(boost_thread ${CMAKE_CURRENT_SOURCE_DIR}/libs/thread)
target_compile_options(boost_log PRIVATE -march=native -msse3 -mavx2)
target_compile_definitions(boost_log PRIVATE _XOPEN_SOURCE=600)
endif()

set(BOOST_LIBRARIES ${LOCAL_BOOST_LIBRARIES} PARENT_SCOPE)
