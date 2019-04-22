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

## Notes

### Async
The ServerContext and ClientContext should be declared before the rpc call or in an outer scope.
This is due to rpc destruction needing the context for reasons it seems to be writing to it.

Basically required to use asio style callbacks where you save shared_ptrs to extend the life of the object(rpc call).
This is due to the completion queue not necessarily finishing a call in the order that you probably want.
So the life of the call has to be extended until all tags are attended to.

Instead of managing state it is much easier to use a new object as a tag and then just delete it when it's used.
This allows multiple calls from clients.
It's also makes the `ok` variable easier to use since it reduces the scope of what it means.

`ok` failure is quite ambiguous, i.e. for Read() it could mean that the client sent WritesDone() or that the client closed the call or the completion queue failed it.

completion queue shutdown sucks.
Program fails with an error if work is enqueued after this is called.
It's likely that you would have to protect all rpc calls with a shutdown mutex and lock on every call.



