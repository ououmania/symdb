CMAKE_MINIMUM_REQUIRED(VERSION 3.7)

set(TARGET symdb)

find_library(Clang_LIBRARY NAMES clang)

add_symbol_executable(${TARGET} ${Clang_LIBRARY} ${LEVELDB_LIBRARY} snappy)

add_custom_target( copy_symdb_config
                   COMMAND cmake -E copy
                   ${CMAKE_CURRENT_SOURCE_DIR}/Config.xml
                   ${CMAKE_CURRENT_BINARY_DIR}/ )

add_dependencies( ${TARGET} copy_symdb_config )
