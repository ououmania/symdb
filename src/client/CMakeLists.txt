CMAKE_MINIMUM_REQUIRED(VERSION 3.7)

find_library(Readline_LIBRARY NAMES readline)

set(TARGET symcli)

add_symbol_executable(${TARGET} ${Readline_LIBRARY})

INSTALL(TARGETS ${TARGET} DESTINATION bin)
