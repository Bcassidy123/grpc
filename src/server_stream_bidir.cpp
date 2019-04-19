#include <atomic>
#include <cassert>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>

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

class CallData : public std::enable_shared_from_this<CallData> {
public:
	CallData(helloworld::Greeter::AsyncService *service,
					 grpc::ServerCompletionQueue *cq)
			: service(service), cq(cq), stream(&context) {}
	void Start() {
		context.AsyncNotifyWhenDone(OnDone());
		service->RequestSayHelloBidir(&context, &stream, cq, cq, OnCreate());
	}

private:
	void Write(HelloReply reply) {
		std::lock_guard l{write_mutex};
		writes.emplace_back(std::move(reply));
		if (writes.size() == 1) {
			stream.Write(writes.front(), OnWrite());
		}
	}

private:
	Handler *OnCreate() {
		return new Handler([this, me = shared_from_this()](bool ok) noexcept {
			std::make_shared<CallData>(service, cq)->Start();
			if (ok) {
				std::cout << std::this_thread::get_id() << " created" << std::endl;
				stream.SendInitialMetadata(OnSendInitialMetadata());
			} else {
				std::cout << std::this_thread::get_id() << " created error"
									<< std::endl;
			}
		});
	}
	Handler *OnSendInitialMetadata() {
		return new Handler([this, me = shared_from_this()](bool ok) noexcept {
			if (ok) {
				std::cout << std::this_thread::get_id() << " sent metadata "
									<< std::endl;
				stream.Read(&request, OnRead());
			} else {
				std::cout << std::this_thread::get_id() << " send metadata error"
									<< std::endl;
			}
		});
	}
	Handler *OnRead() {
		return new Handler([this, me = shared_from_this()](bool ok) noexcept {
			if (ok) {
				std::cout << std::this_thread::get_id() << " read: " << request.name()
									<< std::endl;
				HelloReply reply;
				reply.set_message("You sent: " + request.name());
				stream.Read(&request, OnRead());
				Write(std::move(reply));
			} else {
				std::cout << std::this_thread::get_id() << " read done" << std::endl;
			}
		});
	}
	Handler *OnWrite() {
		return new Handler([this, me = shared_from_this()](bool ok) noexcept {
			if (ok) {
				std::lock_guard l{write_mutex};
				std::cout << std::this_thread::get_id()
									<< " wrote: " << writes.front().message() << std::endl;
				writes.pop_front();
				if (!writes.empty()) {
					stream.Write(writes.front(), OnWrite());
				}
			} else {
				std::cout << std::this_thread::get_id() << " write done" << std::endl;
			}
		});
	}
	Handler *OnFinish() {
		return new Handler([this, me = shared_from_this()](bool ok) noexcept {
			if (ok) {
				std::cout << std::this_thread::get_id()
									<< " finished: " << status.error_code() << " "
									<< status.error_details() << std::endl;
			} else {
				std::cout << std::this_thread::get_id() << " finished error "
									<< std::endl;
			}
		});
	}
	Handler *OnDone() {
		return new Handler([this, me = shared_from_this()](bool ok) noexcept {
			std::cout << std::this_thread::get_id() << " done "
								<< (context.IsCancelled() ? "cancelled" : "") << std::endl;
		});
	}

private:
	helloworld::Greeter::AsyncService *service;
	grpc::ServerCompletionQueue *cq;
	grpc::ServerAsyncReaderWriter<HelloReply, HelloRequest> stream;
	grpc::ServerContext context;
	grpc::Status status;
	HelloRequest request;
	std::list<HelloReply> writes;
	std::mutex write_mutex;
};

class ServerImpl {
	std::string server_address;
	helloworld::Greeter::AsyncService service;
	std::unique_ptr<Server> server;
	std::unique_ptr<grpc::ServerCompletionQueue> cq;

public:
	ServerImpl(std::string server_address) : server_address(server_address) {}
	void Run() {
		ServerBuilder builder;
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(&service);
		cq = builder.AddCompletionQueue();
		server = builder.BuildAndStart();
		std::cout << "Server listening on " << server_address << std::endl;
		HandleRpcs();
	}

private:
	void HandleRpcs() {
		std::atomic_bool shutdown = false;
		std::make_shared<CallData>(&service, cq.get())->Start();
		auto f = [&]() {
			void *tag;
			bool ok;
			while (cq->Next(&tag, &ok)) {
				std::cout << "ok: " << ok << std::endl;
				if (!shutdown)
					static_cast<Handler *>(tag)->Proceed(ok);
			}
		};

		std::thread t1(f);
		std::thread t2(f);
		std::thread t3(f);
		std::thread t4(f);

		std::string j;
		std::cin >> j;
		shutdown = true;
		server->Shutdown();
		cq->Shutdown();
		f();

		t1.join();
		t2.join();
		t3.join();
		t4.join();

		std::cout << " GOOD" << std::endl;
		std::cin >> j;
	}
};

int main() {
	std::string server_address("0.0.0.0:50051");
	ServerImpl server(server_address);
	server.Run();
	return 0;
}