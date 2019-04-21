#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>

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

class SayHelloBidirClient
		: public std::enable_shared_from_this<SayHelloBidirClient> {

public:
	///  Construct a new Say Hello Bidir Client object
	///
	/// @param channel Used in the creation of the stub when starting
	/// @param cq call completion queue
	SayHelloBidirClient(std::shared_ptr<Channel> channel,
											grpc::CompletionQueue *cq)
			: channel(channel), cq(cq) {}

	/// Two stage initialization because shared_from_this is used.
	void Start() {
		stub = Greeter::NewStub(channel);
		stream = stub->AsyncSayHelloBidir(&context, cq, OnCreate());
	}

public:
	void Write(std::string something) {
		std::lock_guard l{pending_requests_mutex};
		HelloRequest request;
		request.set_name(something);
		pending_requests.push_back(std::move(request));
		if (pending_requests.size() == 1) { // there weren't any pending
			stream->Write(pending_requests.front(), OnWrite());
		}
	}
	void Quit() {
		std::lock_guard l{pending_requests_mutex};
		quit = true;
		context.TryCancel();
	}

private: //
	Handler *OnCreate() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				stream->ReadInitialMetadata(OnReadInitialMetadata());
				std::cout << std::this_thread::get_id() << " create" << std::endl;
			} else {
				std::cout << std::this_thread::get_id() << " create error" << std::endl;
			}
		});
	}
	Handler *OnReadInitialMetadata() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				stream->Read(&reply, OnRead());
				std::cout << std::this_thread::get_id() << " read metadata"
									<< std::endl;
			} else {
				std::cout << std::this_thread::get_id() << " read metadata error"
									<< std::endl;
			}
		});
	}
	Handler *OnRead() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			if (ok) {
				std::cout << std::this_thread::get_id() << " read: " << reply.message()
									<< std::endl;
				if (!quit)
					stream->Read(&reply, OnRead());
			} else {
				std::cout << std::this_thread::get_id() << " read error " << std::endl;
			}
		});
	}
	Handler *OnWrite() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			std::lock_guard l{pending_requests_mutex};
			if (ok) {
				std::cout << std::this_thread::get_id()
									<< " write: " << pending_requests.front().name() << std::endl;
				pending_requests.pop_front();
				if (!quit && !pending_requests.empty()) {
					stream->Write(pending_requests.front(), OnWrite());
				}
			} else {
				std::cout << std::this_thread::get_id() << " write error " << std::endl;
			}
		});
	}
	Handler *OnWritesDone() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			std::lock_guard l{pending_requests_mutex};
			pending_requests.clear();
			if (ok) {
				std::cout << std::this_thread::get_id() << " write done " << std::endl;
			} else {
				std::cout << std::this_thread::get_id() << " write done error"
									<< std::endl;
			}
		});
	}
	Handler *OnFinish() {
		return new Handler([this, me = shared_from_this()](bool ok) {
			std::cout << std::this_thread::get_id()
								<< " finished: " << status.error_code() << " "
								<< status.error_details() << " " << status.error_message()
								<< std::endl;
		});
	}

private:
	std::shared_ptr<Channel> channel;
	grpc::CompletionQueue *cq;
	std::unique_ptr<Greeter::Stub> stub;
	grpc::ClientContext context;
	std::unique_ptr<grpc::ClientAsyncReaderWriter<HelloRequest, HelloReply>>
			stream;
	grpc::Status status;
	HelloReply reply;
	std::list<HelloRequest> pending_requests;
	std::mutex pending_requests_mutex;
	bool quit = false;
};

int main() {
	std::string server_address("localhost:50051");

	auto channel =
			grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
	CompletionQueue cq;
	auto p = std::make_shared<SayHelloBidirClient>(channel, &cq);
	auto c = p.get();
	c->Start();
	p.reset();

	auto f = [&]() {
		void *tag;
		bool ok = false;
		while (cq.Next(&tag, &ok)) {
			std::cout << "ok: " << ok << std::endl;
			static_cast<Handler *>(tag)->Proceed(ok);
		}
	};

	std::thread t1(f);
	std::thread t2(f);
	std::thread t3(f);
	std::thread t4(f);
	std::string j;
	while (std::cin >> j) {
		if (j == "end") {
			c->Quit();
			break;
		}
		c->Write(j);
	}
	cq.Shutdown();
	t1.join();
	t2.join();
	t3.join();
	t4.join();

	std::cout << " Good" << std::endl;

	return 0;
}