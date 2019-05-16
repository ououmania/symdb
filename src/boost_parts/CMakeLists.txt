CMAKE_MINIMUM_REQUIRED(VERSION 3.7)

set(BOOST_INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} PARENT_SCOPE)

function(add_boost_lib target src_dir)
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

    set(BOOST_LIBRARIES ${BOOST_LIBRARIES} ${target} PARENT_SCOPE )
endfunction(add_boost_lib)

add_boost_lib(boost_filesystem ${CMAKE_CURRENT_SOURCE_DIR}/libs/filesystem)
add_boost_lib(boost_log ${CMAKE_CURRENT_SOURCE_DIR}/libs/log)
add_boost_lib(boost_system ${CMAKE_CURRENT_SOURCE_DIR}/libs/system)
add_boost_lib(boost_thread ${CMAKE_CURRENT_SOURCE_DIR}/libs/thread)

target_compile_options(boost_log PRIVATE -march=native -msse3 -mavx2)
target_compile_definitions(boost_log PRIVATE _XOPEN_SOURCE=600)

