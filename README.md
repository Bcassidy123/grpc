# grpc
grpc helloworld example with cmake

## Develop
```Shell
vcpkg install grpc
git clone https://github.com/bcassidy123/grpc && cd grpc
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_TOOLCHAIN_FILE=$VCPKG_PATH 
cmake --build . --config Debug
```