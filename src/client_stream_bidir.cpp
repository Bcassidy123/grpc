#include <atomic>
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

class CallData {
	std::unique_ptr<grpc::ClientAsyncReaderWriter<HelloRequest, HelloReply>>
			stream;
	grpc::ClientContext context;
	grpc::Status status;
	HelloReply reply;
	std::list<HelloRequest> pending_requests;
	std::mutex pending_requests_mutex;
	std::atomic_bool finished = false;

public:
	CallData(std::shared_ptr<Channel> channel, grpc::CompletionQueue *cq) {
		auto stub = Greeter::NewStub(channel);
		stream = stub->PrepareAsyncSayHelloBidir(&context, cq);
		stream->StartCall(&OnCreate);
	}

public:
	void Write(std::string something) {
		std::lock_guard l{pending_requests_mutex};
		HelloRequest request;
		request.set_name(something);
		pending_requests.push_back(std::move(request));
		if (pending_requests.size() == 1) { // there weren't any pending
			stream->Write(pending_requests.front(), &OnWrite);
		}
	}
	void Quit() {
		std::lock_guard l{pending_requests_mutex};
		if (pending_requests.empty()) {
			context.TryCancel();
		}
	}

private: //
	Handler OnCreate = [this](bool ok) noexcept {
		if (ok) {
			stream->ReadInitialMetadata(&OnReadInitialMetadata);
			std::cout << std::this_thread::get_id() << " create" << std::endl;
		} else {
			std::cout << std::this_thread::get_id() << " create error" << std::endl;
		}
	};
	Handler OnReadInitialMetadata = [this](bool ok) noexcept {
		if (ok) {
			stream->Read(&reply, &OnRead);
			std::cout << std::this_thread::get_id() << " read metadata" << std::endl;
		} else {
			std::cout << std::this_thread::get_id() << " read metadata error"
								<< std::endl;
		}
	};
	Handler OnRead = [this](bool ok) noexcept {
		if (ok) {
			stream->Read(&reply, &OnRead);
			std::cout << std::this_thread::get_id() << " read: " << reply.message()
								<< std::endl;
		} else {
			std::cout << std::this_thread::get_id() << " read error " << std::endl;
		}
	};
	Handler OnWrite = [this](bool ok) noexcept {
		std::lock_guard l{pending_requests_mutex};
		if (ok) {
			std::cout << std::this_thread::get_id()
								<< " write: " << pending_requests.front().name() << std::endl;
			pending_requests.pop_front();
			if (finished) {
				stream->WritesDone(&OnWritesDone);
			}
			if (!pending_requests.empty()) {
				stream->Write(pending_requests.front(), &OnWrite);
			}
		} else {
			std::cout << std::this_thread::get_id() << " write error " << std::endl;
			pending_requests.clear();
		}
	};
	Handler OnWritesDone = [this](bool ok) noexcept {
		std::lock_guard l{pending_requests_mutex};
		pending_requests.clear();
		if (ok) {
			std::cout << std::this_thread::get_id() << " write done " << std::endl;
		} else {
			std::cout << std::this_thread::get_id() << " write done error"
								<< std::endl;
		}
	};
	Handler OnFinish = [this](bool ok) noexcept {
		std::cout << std::this_thread::get_id()
							<< " finished: " << status.error_code() << " "
							<< status.error_details() << " " << status.error_message()
							<< std::endl;
	};
};

int main() {
	std::string server_address("localhost:50051");

	auto channel =
			grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
	CompletionQueue cq;
	std::unique_ptr<CallData> c{new CallData(channel, &cq)};
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
	f();
	std::cout << " Good" << std::endl;

	return 0;
}