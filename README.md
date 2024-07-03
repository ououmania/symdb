# symdb
Add support to jump to symbol definitions in the other translate units.

## Build

### Prerequisites
libclang and llvm are required to parse the c++ source files. readline is used
by symcli to interact with the server for troubleshooting. They can be installed
from the distribution repos. For example, run the following commands under
centos:
```
yum install -y clang-devel llvm-devel readline-devel
```
Alternatively, you can install them on your own.
### Build
If clang is installed in the standard path, build with the following command:

`cmake -S . -B build`

Otherwise, you can specify clang's root directory:

`cmake -S . -B build CLANG_ROOT=/path/to/clang/root`

## Run
