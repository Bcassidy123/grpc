cmake_minimum_required(VERSION 3.12)

project(GRPCTest
	LANGUAGES
		CXX
	VERSION
		0.1.0
)

# hard time compiling without this thing
add_definitions(-D_WIN32_WINNT=0x600)

find_package(gRPC CONFIG REQUIRED)
find_program(PROTOC protoc)
if(NOT PROTOC)
	message(FATAL_ERROR "protoc not found!")
endif()
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin)
if(NOT GRPC_CPP_PLUGIN)
		message(FATAL_ERROR "grpc_cpp_plugin not found!")
endif()

set(PROTOS_DIR ${CMAKE_CURRENT_LIST_DIR}/protos)
set(GENERATED_DIR ${CMAKE_CURRENT_LIST_DIR}/generated)

# proto output
set(PROTO_STUBS
	${GENERATED_DIR}/helloworld.grpc.pb.cc
	${GENERATED_DIR}/helloworld.grpc.pb.h
	${GENERATED_DIR}/helloworld.pb.cc
	${GENERATED_DIR}/helloworld.pb.h
)
add_custom_command(OUTPUT
	${PROTO_STUBS}
	COMMAND ${CMAKE_COMMAND} -E make_directory ${GENERATED_DIR}
	COMMAND ${PROTOC} 
		-I ${PROTOS_DIR}
		--plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
		--grpc_out=${GENERATED_DIR}
		${PROTOS_DIR}/helloworld.proto
	COMMAND ${PROTOC}
		-I ${PROTOS_DIR}
		--cpp_out=${GENERATED_DIR}
		${PROTOS_DIR}/helloworld.proto
	MAIN_DEPENDENCY
		${PROTOS_DIR}/helloworld.proto
)

# grpc stubs
add_library(helloworld_LIB 
	STATIC
		${PROTO_STUBS}
)
target_include_directories(helloworld_LIB
	PUBLIC
		${GENERATED_DIR}
)
target_link_libraries(helloworld_LIB
	PUBLIC
		gRPC::gpr
		gRPC::grpc
		gRPC::grpc++
)

# Async Server
add_executable(server
	src/server.cpp
)
target_compile_features(server
	PRIVATE
		cxx_std_17
)
target_link_libraries(server
	PRIVATE
		helloworld_LIB
)

# Sync Server
add_executable(server_sync
	src/server_sync.cpp
)
target_compile_features(server_sync
	PRIVATE
		cxx_std_17
)
target_link_libraries(server_sync
	PRIVATE
		helloworld_LIB
)

# Server stream
add_executable(server_stream
	src/server_stream.cpp
)
target_compile_features(server_stream
	PRIVATE
		cxx_std_17
)
target_link_libraries(server_stream
	PRIVATE
		helloworld_LIB
)

# Server stream client
add_executable(server_stream_client
	src/server_stream_client.cpp
)
target_compile_features(server_stream_client
	PRIVATE
		cxx_std_17
)
target_link_libraries(server_stream_client
	PRIVATE
		helloworld_LIB
)

# Server stream bidir
add_executable(server_stream_bidir
	src/server_stream_bidir.cpp
)
target_compile_features(server_stream_bidir
	PRIVATE
		cxx_std_17
)
target_link_libraries(server_stream_bidir
	PRIVATE
		helloworld_LIB
)
# Async Client
add_executable(client
	src/client.cpp
)
target_compile_features(client
	PRIVATE
		cxx_std_17
)
target_link_libraries(client
	PRIVATE
		helloworld_LIB
)

# Sync client
add_executable(client_sync
	src/client_sync.cpp
)
target_compile_features(client_sync
	PRIVATE
		cxx_std_17
)
target_link_libraries(client_sync
	PRIVATE
		helloworld_LIB
)

# Client Stream
add_executable(client_stream
	src/client_stream.cpp
)
target_compile_features(client_stream
	PRIVATE
		cxx_std_17
)
target_link_libraries(client_stream
	PRIVATE
		helloworld_LIB
)

# Client Stream Client
add_executable(client_stream_client
	src/client_stream_client.cpp
)
target_compile_features(client_stream_client
	PRIVATE
		cxx_std_17
)
target_link_libraries(client_stream_client
	PRIVATE
		helloworld_LIB
)

# Client Stream Bidir
add_executable(client_stream_bidir
	src/client_stream_bidir.cpp
)
target_compile_features(client_stream_bidir
	PRIVATE
		cxx_std_17
)
target_link_libraries(client_stream_bidir
	PRIVATE
		helloworld_LIB
)