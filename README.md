# symdb
Add support to jump to symbol definitions in the other translate units. It works
well as a backend server for YouCompleteMe. If YCM requires the definition
location of a symbol and what libclang gives is not, it will communicate with
symdb to get the definition. Therefore the naming style of the files and code is
consistent with YCM.

## Build

### Prerequisites
A compiler with at least C++14 is required. C++20 support is better. We will
switch to some boost modules such as filesystem if no c++20 available. But
remember it's a headache to make any dependency on the boost libraries.
libclang is required to parse the c++ source files. readline is used by symcli
to interact with the server for troubleshooting. They can be installed from the
distribution repos. For example, run the following commands under
centos:
```
yum install -y clang-devel readline-devel
```
Alternatively, you can install them on your own.
### Build
If clang is installed in the standard path, build with the following command:

`cmake -S . -B build`

Otherwise, you need to specify clang's root directory:

`cmake -S . -B build CLANG_ROOT=/path/to/clang/root`

## Run
symdb needs a compilation database to parse the compilation flags of translation
units. For projects using cmake, just add the following line to your top-level
CMakeLists.txt:
```
set( CMAKE_EXPORT_COMPILE_COMMANDS 1 )
```
You need to do a little configuration before running the server. Please refer to
src/server/Symdb.xml.
```
./symdb -c <config-file>
```
You can get the full command line options by:
```
./symdb -h
```
