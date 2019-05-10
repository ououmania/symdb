CMAKE_MINIMUM_REQUIRED(VERSION 3.7)

find_library(LevelDB_LIBRARY NAMES leveldb)
find_library(Clang_LIBRARY NAMES clang)

add_symbol_executable(symdb snappy ${LevelDB_LIBRARY})
