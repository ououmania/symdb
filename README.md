# symdb
Add support to jump to symbol definitions in the other translate units.

## Build

clang-devel is required to compile the code. If it's installed in the standard
path, build with the following command:

`cmake -S . -B build`

Otherwisze, you can specify clang's root directory:

`cmake -S . -B build CLANG_ROOT=/path/to/clang/root`

## Run
