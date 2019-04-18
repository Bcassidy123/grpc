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

template <typename W, typename R, bool Threadsafe = true>
class ServerAsyncReaderWriter {
protected:
	virtual ~ServerAsyncReaderWriter() {}
	// F is void(grpc::ServerAyncReaderWriter *stream, grpc::context *context,
	// void *tag) which requests the stream from the service
	template <typename F>
	void Init(grpc::ServerAsyncReaderWriter<W, R> *stream,
						grpc::ServerContext *context, F &&f) noexcept {
		this->stream = stream;
		finished = false;
		num_in_flight = 2;
		context->AsyncNotifyWhenDone(&doner);
		f(stream, context, &creator);
	}
	void SendInitialMetadata() noexcept {
		++num_in_flight;
		stream->SendInitialMetadata(&initial_metadata_sender);
	}
	void Read(R *r) noexcept {
		++num_in_flight;
		stream->Read(r, &reader);
	}
	void Write(W const &w) noexcept {
		++num_in_flight;
		stream->Write(w, &writer);
	}
	void Write(W const &w, grpc::WriteOptions const &opt) noexcept {
		++num_in_flight;
		stream->Write(w, opt, &writer);
	}
	void WriteAndFinish(W const &w, grpc::WriteOptions const &opt,
											grpc::Status const &status) noexcept {
		++num_in_flight;
		stream->WriteAndFinish(w, opt, status, &write_and_finisher);
	}
	void Finish(grpc::Status const &status) noexcept {
		++num_in_flight;
		stream->Finish(status, &finisher);
	}

private:
	virtual void OnCreate(bool ok) noexcept {}
	virtual void OnSendInitialMetadata(bool ok) noexcept {}
	virtual void OnRead(bool ok) noexcept {}
	virtual void OnWrite(bool ok) noexcept {}
	virtual void OnWriteAndFinish(bool ok) noexcept {}
	virtual void OnFinish(bool ok) noexcept {}
	virtual void OnDone(bool ok) noexcept {}
	virtual void OnFinally() noexcept {}

private:
	void Finally() noexcept {
		if (finished && num_in_flight == 0) {
			OnFinally();
		}
	}
	grpc::ServerAsyncReaderWriter<W, R> *stream;
	std::conditional_t<Threadsafe, std::atomic_bool, bool> finished;
	std::conditional_t<Threadsafe, std::atomic_uint, unsigned> num_in_flight;
	Handler creator = [this](bool ok) {
		OnCreate(ok);
		--num_in_flight;
		Finally();
	};
	Handler initial_metadata_sender = [this](bool ok) {
		OnSendInitialMetadata(ok);
		--num_in_flight;
		Finally();
	};
	Handler reader = [this](bool ok) {
		OnRead(ok);
		--num_in_flight;
		Finally();
	};
	Handler writer = [this](bool ok) {
		OnWrite(ok);
		--num_in_flight;
		Finally();
	};
	Handler write_and_finisher = [this](bool ok) {
		OnWriteAndFinish(ok);
		--num_in_flight;
		Finally();
	};
	Handler finisher = [this](bool ok) {
		OnFinish(ok);
		finished = true;
		--num_in_flight;
		Finally();
	};
	Handler doner = [this](bool ok) {
		OnDone(ok);
		finished = true;
		--num_in_flight;
		Finally();
	};
};

class CallData final
		: public ServerAsyncReaderWriter<HelloReply, HelloRequest> {
	using Base = ServerAsyncReaderWriter<HelloReply, HelloRequest>;

public:
	CallData(helloworld::Greeter::AsyncService *service,
					 grpc::ServerCompletionQueue *cq)
			: service(service), cq(cq), stream(&context) {
		Init(&stream, &context, [&](auto *stream, auto *context, void *tag) {
			service->RequestSayHelloBidir(context, stream, cq, cq, tag);
		});
	}

private:
	void OnCreate(bool ok) noexcept override {
		new CallData(service, cq);
		if (ok) {
			std::cout << std::this_thread::get_id() << " created" << std::endl;
			SendInitialMetadata();
		} else {
			std::cout << std::this_thread::get_id() << " created error" << std::endl;
			Finish({grpc::StatusCode::INTERNAL, "Something went wrong"});
		}
	}
	void OnSendInitialMetadata(bool ok) noexcept override {
		if (ok) {
			std::cout << std::this_thread::get_id() << " sent metadata " << std::endl;
			Read(&request);
		} else {
			std::cout << std::this_thread::get_id() << " send metadata error"
								<< std::endl;
			Finish({grpc::StatusCode::INTERNAL, "Something went wrong"});
		}
	}
	void OnRead(bool ok) noexcept override {
		if (ok) {
			std::cout << std::this_thread::get_id() << " read: " << request.name()
								<< std::endl;
			HelloReply reply;
			reply.set_message("You sent: " + request.name());
			Read(&request);
			std::lock_guard l{pending_writes_mutex};
			pending_writes.push_back(reply);
			if (pending_writes.size() == 1) {
				Write(pending_writes.front());
			}
		} else {
			std::cout << std::this_thread::get_id() << " read done" << std::endl;
		}
	}
	void OnWrite(bool ok) noexcept override {
		if (ok) {
			std::lock_guard l{pending_writes_mutex};
			std::cout << std::this_thread::get_id()
								<< " wrote: " << pending_writes.front().message() << std::endl;
			pending_writes.pop_front();
			if (!pending_writes.empty()) {
				Write(pending_writes.front());
			}
		} else {
			std::cout << std::this_thread::get_id() << " write done" << std::endl;
		}
	}
	void OnFinish(bool ok) noexcept override {
		if (ok) {
			std::cout << std::this_thread::get_id()
								<< " finished: " << status.error_code() << " "
								<< status.error_details() << std::endl;
			// done will be called after this
		} else {
			std::cout << std::this_thread::get_id() << " finished error "
								<< std::endl;
		}
	}
	void OnDone(bool ok) noexcept override {
		std::cout << std::this_thread::get_id() << " done "
							<< (context.IsCancelled() ? "cancelled" : "") << std::endl;
		if (context.IsCancelled()) {
			status = Status::CANCELLED;
		}
	}
	void OnFinally() noexcept override { delete this; }

private:
	helloworld::Greeter::AsyncService *service;
	grpc::ServerCompletionQueue *cq;
	grpc::ServerAsyncReaderWriter<HelloReply, HelloRequest> stream;
	grpc::ServerContext context;
	grpc::Status status;
	HelloRequest request;
	std::list<HelloReply> pending_writes;
	std::mutex pending_writes_mutex;
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
		new CallData(&service, cq.get());
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