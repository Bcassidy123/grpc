#include <iostream>
#include <memory>
#include <string>
#include <thread>

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

class SayHellosClientStreamServer
		: public std::enable_shared_from_this<SayHellosClientStreamServer> {
public:
	SayHellosClientStreamServer(helloworld::Greeter::AsyncService *service,
															grpc::ServerCompletionQueue *cq)
			: service(service), cq(cq), stream(&context) {}
	void Start() {
		context.AsyncNotifyWhenDone(OnDone());
		service->RequestSayHellosClient(&context, &stream, cq, cq, OnCreate());
	}
	Handler *OnCreate() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				std::make_shared<SayHellosClientStreamServer>(service, cq)->Start();
				stream.SendInitialMetadata(OnSendInitialMetadata());
			}
		});
	}
	Handler *OnSendInitialMetadata() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				stream.Read(&request, OnReadMessage());
			}
		});
	}
	Handler *OnReadMessage() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				auto m = request.name();
				std::cout << "read: " << m << std::endl;
				msgs.emplace_back(std::move(m));
				stream.Read(&request, OnReadMessage());
			} else {
				// ReadDone
				std::string r = "You sent: ";
				for (auto &&m : msgs) {
					r += m;
				}
				HelloReply reply;
				reply.set_message(r);
				stream.Finish(reply, grpc::Status::OK, OnFinish());
			}
		});
	}
	Handler *OnFinish() {
		return new Handler([me = shared_from_this()](bool ok) {
			std::cout << "SayHellosClient Finish" << std::endl;
		});
	}
	Handler *OnDone() {
		return new Handler([me = shared_from_this()](bool ok) {
			std::cout << "SayHellosClient Done" << std::endl;
		});
	}

private:
	ServerContext context;
	helloworld::Greeter::AsyncService *service;
	grpc::ServerCompletionQueue *cq;
	grpc::ServerAsyncReader<HelloReply, helloworld::HelloRequest> stream;
	HelloRequest request;
	std::vector<std::string> msgs;
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
		server->Wait();
		std::cout << "Server shutdown on " << server_address << std::endl;
	}

private:
	void HandleRpcs(grpc::ServerCompletionQueue *cq) {
		std::make_shared<SayHellosClientStreamServer>(&service, cq)->Start();
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
	server.Run(4);
	return 0;
}