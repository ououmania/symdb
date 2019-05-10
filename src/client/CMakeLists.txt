CMAKE_MINIMUM_REQUIRED(VERSION 3.7)

find_library(Readline_LIBRARY NAMES readline)

add_symbol_executable(symcli ${Readline_LIBRARY})
