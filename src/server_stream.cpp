#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <grpcpp/grpcpp.h>

#include "helloworld.grpc.pb.h"

#include "common.hpp"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class SayHellosServerStreamServer
		: public std::enable_shared_from_this<SayHellosServerStreamServer> {
public:
	SayHellosServerStreamServer(helloworld::Greeter::AsyncService *service,
															grpc::ServerCompletionQueue *cq)
			: service(service), cq(cq), stream(&context) {}
	void Start() {
		context.AsyncNotifyWhenDone(OnDone());
		service->RequestSayHellos(&context, &request, &stream, cq, cq, OnCreate());
	}
	Handler *OnCreate() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				std::make_shared<SayHellosServerStreamServer>(service, cq)->Start();
				stream.SendInitialMetadata(OnSendInitialMetadata());
			}
		});
	}
	Handler *OnSendInitialMetadata() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				HelloReply reply;
				reply.set_message(request.name());
				stream.Write(reply, OnWriteMessage());
			}
		});
	}
	Handler *OnWriteMessage() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				if (--num_messages) {
					HelloReply reply;
					reply.set_message(request.name());
					stream.Write(reply, OnWriteMessage());
				} else {
					stream.Finish(grpc::Status::OK, OnFinish());
				}
			}
		});
	}
	Handler *OnFinish() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				std::cout << "SayHellosServerStreamServer finished " << std::endl;
			}
		});
	}
	Handler *OnDone() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			std::cout << "SayHellosServerStreamServer done" << std::endl;
		});
	}

private:
	Greeter::AsyncService *service;
	grpc::ServerCompletionQueue *cq;
	grpc::ServerContext context;
	grpc::ServerAsyncWriter<HelloReply> stream;
	HelloRequest request;
	int num_messages = 4;
};

class ServerImpl {
	std::string server_address;
	helloworld::Greeter::AsyncService service;
	std::unique_ptr<Server> server;
	std::vector<std::unique_ptr<grpc::ServerCompletionQueue>> cqs;

public:
	ServerImpl(std::string server_address) : server_address(server_address) {}
	void Run(int num_threads = 1) {
		ServerBuilder builder;
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(&service);
		for (auto i = 0; i < num_threads; ++i) {
			cqs.emplace_back(builder.AddCompletionQueue());
		}
		server = builder.BuildAndStart();
		std::cout << "Server listening on " << server_address << std::endl;

		std::vector<std::thread> threads;
		for (auto &&cq : cqs) {
			threads.emplace_back([this, cq = cq.get()] { HandleRpcs(cq); });
		}

		std::string j;
		std::cin >> j;
		server->Shutdown();
		for (auto &&cq : cqs) {
			cq->Shutdown();
		}
		for (auto &&t : threads) {
			t.join();
		}
		std::cout << "Server shutdown on " << server_address << std::endl;
		std::cin >> j;
	}

private:
	void HandleRpcs(grpc::ServerCompletionQueue *cq) {
		std::make_shared<SayHellosServerStreamServer>(&service, cq)->Start();
		void *tag;
		bool ok;
		while (cq->Next(&tag, &ok)) {
			static_cast<Handler *>(tag)->Proceed(ok);
		}
	}
};

int main() {
	std::string server_address("0.0.0.0:50051");
	ServerImpl server(server_address);
	server.Run();
	return 0;
}