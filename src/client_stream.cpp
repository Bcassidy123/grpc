#include <iostream>
#include <list>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "helloworld.grpc.pb.h"

#include "common.hpp"

using grpc::Channel;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class SayHellosServerStreamClient
		: public std::enable_shared_from_this<SayHellosServerStreamClient> {
public:
	SayHellosServerStreamClient(std::shared_ptr<Channel> channel,
															grpc::CompletionQueue *cq, bool *done)
			: cq(cq), done(done) {
		stub = Greeter::NewStub(channel);
	}
	void Start(HelloRequest const &request) {
		stream = stub->AsyncSayHellos(&context, request, cq, OnCreate());
	}
	Handler *OnCreate() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				stream->ReadInitialMetadata(OnReadInitialMetadata());
			}
		});
	}
	Handler *OnReadInitialMetadata() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				stream->Read(&reply, OnRead());
			}
		});
	}
	Handler *OnRead() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				std::cout << "read: " << reply.message() << std::endl;
				stream->Read(&reply, OnRead());
			} else {
				stream->Finish(&status, OnFinish());
			}
		});
	}
	Handler *OnFinish() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				std::cout << "SayHellosServerStreamClient finished with status: "
									<< status.error_code() << std::endl;
			} else {
				std::cout << "SayHellosServerStreamClient finished in error status: "
									<< status.error_code() << std::endl;
			}
			*done = true;
		});
	}

private:
	std::unique_ptr<Greeter::Stub> stub;
	grpc::CompletionQueue *cq;
	grpc::ClientContext context;
	grpc::Status status;
	HelloReply reply;
	bool *done;
	std::unique_ptr<grpc::ClientAsyncReader<HelloReply>> stream;
};
class GreeterClient {
private:
	std::shared_ptr<Channel> channel;
	grpc::CompletionQueue cq;

public:
	GreeterClient(std::string server_address) {
		channel =
				grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
	}

	void SayHelloAsync(const std::string &user) {
		std::thread t([&] {
			HelloRequest request;
			request.set_name(user);
			bool done = false;
			std::make_shared<SayHellosServerStreamClient>(channel, &cq, &done)
					->Start(request);
			void *tag;
			bool ok;
			while (cq.Next(&tag, &ok)) {
				static_cast<Handler *>(tag)->Proceed(ok);
				if (done) {
					done = false;
					cq.Shutdown();
				}
			}
		});
		std::string j;
		std::cin >> j;
		t.join();
	}
};
int main() {
	std::string server_address("localhost:50051");
	GreeterClient greeter(server_address);
	std::string user = "world";
	greeter.SayHelloAsync(user);
	return 0;
}