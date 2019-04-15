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

		//! This should probably be kept in an object and managed like the server
		Status status;
		auto rpc = stub_->PrepareAsyncSayHellosClient(&context, &reply, &cq);
		rpc->StartCall(this);
		void *tag;
		bool ok = false;
		int i = 0;
		bool done = false;
		while (cq.Next(&tag, &ok)) {
			if (!ok || done) // read fail will set ok to false
				break;
			if (i++ < 5) {
				rpc->Write(request, this);
			} else {
				rpc->WritesDone(this);
				done = true;
			}
		}
		rpc->Finish(&status, this);
		cq.Next(&tag, &ok); // To deal with finished stream
		if (!ok)
			return "RPC failed";
		return reply.message();
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