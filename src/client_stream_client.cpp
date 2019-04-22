#include <iostream>
#include <memory>
#include <string>
#include <thread>

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

class SayHellosClientStreamClient
		: public std::enable_shared_from_this<SayHellosClientStreamClient> {
public:
	SayHellosClientStreamClient(std::shared_ptr<Channel> channel,
															grpc::CompletionQueue *cq) {
		stub = Greeter::NewStub(channel);
		stream = stub->PrepareAsyncSayHellosClient(&context, &response, cq);
	}
	void Start(std::list<std::string> msgs) {
		this->msgs = std::move(msgs);
		stream->StartCall(OnCreate());
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
				if (!msgs.empty()) {
					HelloRequest request;
					request.set_name(msgs.front());
					stream->Write(request, OnWriteMessage());
				}
			}
		});
	}

	Handler *OnWriteMessage() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				msgs.pop_front();
				if (!msgs.empty()) {
					HelloRequest request;
					request.set_name(msgs.front());
					stream->Write(request, OnWriteMessage());
				} else {
					stream->WritesDone(OnWritesDone());
				}
			}
		});
	}
	Handler *OnWritesDone() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				stream->Finish(&status, OnFinish());
			}
		});
	}
	Handler *OnFinish() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				std::cout << "SayHellosClientStream finished with status: "
									<< status.error_code() << std::endl;
				std::cout << "read: " << response.message() << std::endl;
			}
		});
	}

private:
	///  ClientContext MUST outlive the rpc so it must be declared before stream
	grpc::ClientContext context;
	std::unique_ptr<Greeter::Stub> stub;
	std::unique_ptr<grpc::ClientAsyncWriter<HelloRequest>> stream;
	grpc::Status status;
	HelloReply response;
	std::list<std::string> msgs;
};

class ClientImpl {
public:
	ClientImpl(std::string server_address) : server_address(server_address) {}
	void Run() {
		channel =
				grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
		stub_ = Greeter::NewStub(channel);
		std::thread t([this] { HandleRpcs(&cq); });
		// HandleRpcs(&cq);
		std::string j;
		std::cin >> j;
		cq.Shutdown();
		t.join();
	}

private:
	void HandleRpcs(grpc::CompletionQueue *cq) {
		std::make_shared<SayHellosClientStreamClient>(channel, cq)
				->Start({"what", "in", "the", "world"});
		void *tag;
		bool ok = false;
		while (cq->Next(&tag, &ok)) {
			static_cast<Handler *>(tag)->Proceed(ok);
		}
	}

private:
	std::string server_address;
	std::shared_ptr<Channel> channel;
	CompletionQueue cq;
	std::unique_ptr<Greeter::Stub> stub_;
};
int main() {
	std::string server_address("localhost:50051");
	ClientImpl client(server_address);
	client.Run();
	return 0;
}