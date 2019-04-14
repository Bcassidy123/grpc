#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "helloworld.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class GreeterClient {
private:
  std::unique_ptr<Greeter::Stub> stub_;

public:
  GreeterClient(std::shared_ptr<Channel> channel)
      : stub_(Greeter::NewStub(channel)) {}

  std::string SayHello(const std::string &user) {
    HelloRequest request;
    request.set_name(user);
    HelloReply reply;
    ClientContext context;
    /* {
    Status status = stub_->SayHello(&context, request, &reply);
    if (status.ok()) {
    return reply.message();
    }
    std::cout << status.error_code() << ": " << status.error_message()
      << std::endl;
        }*/

    {
      CompletionQueue cq;
      std::unique_ptr<grpc::ClientAsyncResponseReader<HelloReply>> rpc(
          stub_->AsyncSayHello(&context, request, &cq));
      Status status;
      rpc->Finish(&reply, &status, (void *)1);
      void *got_tag;
      bool ok = false;
      // call blocks
      std::chrono::system_clock::time_point tp =
          std::chrono::system_clock::now() + std::chrono::milliseconds(3000);
      if (cq.Next(&got_tag, &ok)) {
        if (ok && got_tag == (void *)1) {
          /// check reply and status
          if (status.ok()) {
            return reply.message();
          }
          std::cout << status.error_code() << ": " << status.error_message()
                    << std::endl;
        }
      }
    }
    return "RPC failed";
  }
};
int main() {
  std::string server_address("localhost:50051");
  GreeterClient greeter(
      grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials()));
  std::string user = "world";
  std::string reply = greeter.SayHello(user);
  std::cout << "Greeter received: " << reply << std::endl;
  return 0;
}