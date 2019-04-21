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

/// rpc call
class SayHelloBidirServer
		: public std::enable_shared_from_this<SayHelloBidirServer> {
public:
	/// Construct a new Say Hello Bidir object
	///
	/// Parameters are stored and will be used to initiate wait for a new rpc when
	/// this one is used.
	///
	/// @param service Used to wait for the call
	/// @param call_cq Completes everything after call (read, write, finish, done,
	/// etc...)
	/// @param notification_cq Completes when a call is initiated
	SayHelloBidirServer(helloworld::Greeter::AsyncService *service,
											grpc::CompletionQueue *call_cq,
											grpc::ServerCompletionQueue *notification_cq)
			: service(service), call_cq(call_cq), notification_cq(notification_cq),
				stream(&context) {}
	/// Two stage initialization because shared_from_this is used.
	void Start() {
		// Both OnDone() and OnCreate() make new Handlers which store a reference to
		// this shared ptr. So once Start() is called references can be dropped.

		// When the rpc ends Done is called. This is always called.
		context.AsyncNotifyWhenDone(OnDone());
		// Initiate a wait for rpc
		service->RequestSayHelloBidir(&context, &stream, call_cq, notification_cq,
																	OnCreate());
	}

private:
	void Write(HelloReply reply) {
		// If we're using more than one thread on the completion queue then we'd
		// have a data race on the writes.
		std::lock_guard l{write_mutex};
		// Only add the the pending writes as an ongoing write will get to it
		// eventually.
		writes.emplace_back(std::move(reply));
		// If there weren't any pending writes then we'll have to start the write.
		if (writes.size() == 1) {
			stream.Write(writes.front(), OnWrite());
		}
	}

private: // Handlers
	// Handlers are deleted after use. This destroys the shared_ptr reference and
	// so new Handlers need to be created to keep the object alive if it's
	// required. The order that concurrent handlers complete is non deterministic.
	// Even if Done is called, other handlers might still be inflight and there is
	// no guarantee on their success or failure so the object that keeps the state
	// must still be kept alive.

	Handler *OnCreate() {
		return new Handler([this, me = shared_from_this()](bool ok) noexcept {
			if (ok) {
				std::cout << std::this_thread::get_id() << " created" << std::endl;
				// Create another waiter for this rpc
				std::make_shared<SayHelloBidirServer>(service, call_cq, notification_cq)
						->Start();
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
				// Begin read
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
				// Continue to read until failure
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
				// There can only be one write at a time and so writes get queued.
				// Thus continue to write until the queue is empty.
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
	grpc::CompletionQueue *call_cq;
	grpc::ServerCompletionQueue *notification_cq;
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
	std::vector<std::unique_ptr<grpc::ServerCompletionQueue>> cqs;

public:
	ServerImpl(std::string server_address) : server_address(server_address) {}
	void Run() {
		ServerBuilder builder;
		builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
		builder.RegisterService(&service);
		for (auto i = 0; i < 4; ++i) {
			cqs.emplace_back(builder.AddCompletionQueue());
		}
		server = builder.BuildAndStart();
		std::cout << "Server listening on " << server_address << std::endl;

		// Only using one completion queue for both notification and calls.
		// Unless it's really necessary you probably want this.
		auto f = [](helloworld::Greeter::AsyncService *service,
								grpc::ServerCompletionQueue *cq) {
			return [service, cq] {
				std::make_shared<SayHelloBidirServer>(service, cq, cq)->Start();
				void *tag;
				bool ok;
				while (cq->Next(&tag, &ok)) {
					static_cast<Handler *>(tag)->Proceed(ok);
				}
			};
		};

		// The recommendation is for one thread per completion queue
		std::vector<std::thread> ts;
		for (auto &cq : cqs) {
			ts.emplace_back(f(&service, cq.get()));
		}

		std::string j;
		std::cin >> j;
		// Server shutdown must be done before completion queue.
		server->Shutdown();
		// Initiate completion queue shutdowns.
		for (auto &cq : cqs) {
			cq->Shutdown();
		}
		// Completion queues have to be drained after shutdown so we'll wait for the
		// threads to complete.
		for (auto &t : ts) {
			t.join();
		}

		std::cout << "Finished" << std::endl;
	}
};

int main() {
	std::string server_address("0.0.0.0:50051");
	ServerImpl server(server_address);
	server.Run();
	return 0;
}