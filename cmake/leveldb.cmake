set(LEVELDB_LIBRARY_NAME leveldb)

if(NOT USE_SYSTEM_LEVELDB)
    option(LEVELDB_BUILD_TESTS "Build LevelDB's unit tests" OFF)
    option(LEVELDB_BUILD_BENCHMARKS "Build LevelDB's benchmarks" OFF)
    option(LEVELDB_INSTALL "Install LevelDB's header and library" OFF)
    if(NOT LEVELDB_ROOT_DIR)
        set(LEVELDB_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/leveldb)
    endif()

    if(EXISTS "${LEVELDB_ROOT_DIR}/CMakeLists.txt")
        add_subdirectory(${LEVELDB_ROOT_DIR}/ third_party/leveldb)
        if(TARGET ${LEVELDB_LIBRARY_NAME})
            set(LEVELDB_LIBRARY ${LEVELDB_LIBRARY_NAME})
        endif()
        set(LEVELDB_INCLUDE_DIR "${LEVELDB_ROOT_DIR}/include")
    else()
        message(WARNING "LEVELDB_ROOT_DIR is wrong")
    endif()
else()
    find_path(LEVELDB_INCLUDE_DIR NAMES "leveldb")
    find_library(LEVELDB_LIBRARY NAMES ${LEVELDB_LIBRARY_NAME})
endif()
