CMAKE_MINIMUM_REQUIRED(VERSION 3.7)

set(TARGET symdb)

find_library(Clang_LIBRARY NAMES clang)

add_symbol_executable(${TARGET}
                      ${Clang_LIBRARY}
                      ${LEVELDB_LIBRARY}
                      ${SNAPPY_LIBRARY}
                      ${PUGIXML_LIBRARY} )

set(symdb_config_file ${CMAKE_CURRENT_SOURCE_DIR}/Symdb.xml)

add_custom_target( copy_symdb_config
                   COMMAND cmake -E copy
                   ${symdb_config_file}
                   ${CMAKE_CURRENT_BINARY_DIR}/ )

add_dependencies( ${TARGET} copy_symdb_config )

INSTALL(TARGETS ${TARGET} DESTINATION bin)
INSTALL(FILES ${symdb_config_file} DESTINATION bin)
