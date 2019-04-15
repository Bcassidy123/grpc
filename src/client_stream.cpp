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
		CompletionQueue cq;
		std::string all = "";

		//! This should probably be kept in an object and managed like the server
		Status status;
		auto rpc = stub_->PrepareAsyncSayHellos(&context, request, &cq);
		rpc->StartCall(this);
		void *tag;
		bool ok = false;
		while (cq.Next(&tag, &ok)) {
			if (!ok) // read fail will set ok to false
				break;
			rpc->Read(&reply, this);
			all += reply.message();
		}
		rpc->Finish(&status, this); // Finish the stream
		cq.Next(&tag, &ok);					// To deal with finished stream
		if (!ok)
			return "RPC failed";
		return all;
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