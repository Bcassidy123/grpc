#include <iostream>
#include <memory>
#include <string>
#include <utility>

#include <windows.h>

#include <grpcpp/grpcpp.h>

#include "helloworld.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class GreeterServiceImpl final : public Greeter::Service {
  Status SayHello(ServerContext *context, const HelloRequest *request,
                  HelloReply *reply) override {
    std::string prefix("Hello ");
    reply->set_message(prefix + request->name());
    return Status::OK;
  }
};

class ServerImpl {
private:
  std::string server_address;
  GreeterServiceImpl service;
  std::unique_ptr<Server> server;

public:
  ServerImpl(std::string server_address)
      : server_address(std::move(server_address)) {}

public:
  void Run() {
    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
    std::cout << "Server listening on " << server_address << std::endl;
    server->Wait();
  }
};
int main() {
  std::string server_address("0.0.0.0:50051");
  ServerImpl server(server_address);
  server.Run();
  return 0;
}