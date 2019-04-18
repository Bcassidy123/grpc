#include <atomic>
#include <cassert>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <string>

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

template <typename W, typename R> class ServerAsyncReaderWriter {
protected:
	ServerAsyncReaderWriter(grpc::ServerAsyncReaderWriter<W, R> *stream = nullptr,
													grpc::ServerContext *context = nullptr) noexcept
			: stream(stream), context(context) {}
	void Init(grpc::ServerAsyncReaderWriter<W, R> *stream,
						grpc::ServerContext *context) noexcept {
		this->stream = stream;
		this->context = context;
	}
	void SendInitialMetadata() noexcept {
		stream->SendInitialMetadata(&initial_metadata_sender);
	}
	void Read(R *r) noexcept { stream->Read(r, &reader); }
	void Write(W const &w) noexcept { stream->Write(w, &writer); }
	void Write(W const &w, grpc::WriteOptions const &opt) noexcept {
		stream->Write(w, opt, &writer);
	}
	void WriteAndFinish(W const &w, grpc::WriteOptions const &opt,
											grpc::Status const &status) noexcept {
		stream->WriteAndFinish(w, opt, status, &write_and_finisher);
	}
	void Finish(grpc::Status const &status) noexcept {
		stream->Finish(status, &finisher);
	}
	void *RequestTag() noexcept { return &creator; }
	void *DoneTag() noexcept { return &doner; }

private:
	virtual void OnCreate(bool ok) noexcept {}
	virtual void OnSendInitialMetadata(bool ok) noexcept {}
	virtual void OnRead(bool ok) noexcept {}
	virtual void OnWrite(bool ok) noexcept {}
	virtual void OnWriteAndFinish(bool ok) noexcept {}
	virtual void OnFinish(bool ok) noexcept {}
	virtual void OnDone(bool ok) noexcept {}

private:
	grpc::ServerAsyncReaderWriter<W, R> *stream;
	grpc::ServerContext *context;
	Handler creator = [this](bool ok) { OnCreate(ok); };
	Handler initial_metadata_sender = [this](bool ok) {
		OnSendInitialMetadata(ok);
	};
	Handler reader = [this](bool ok) { OnRead(ok); };
	Handler writer = [this](bool ok) { OnWrite(ok); };
	Handler write_and_finisher = [this](bool ok) { OnWriteAndFinish(ok); };
	Handler finisher = [this](bool ok) { OnFinish(ok); };
	Handler doner = [this](bool ok) { OnDone(ok); };
};

class CallData final
		: public ServerAsyncReaderWriter<HelloReply, HelloRequest> {
	using Base = ServerAsyncReaderWriter<HelloReply, HelloRequest>;

public:
	CallData(helloworld::Greeter::AsyncService *service,
					 grpc::ServerCompletionQueue *cq)
			: Base(&stream, &context), service(service), cq(cq), stream(&context) {
		context.AsyncNotifyWhenDone(Base::DoneTag());
		service->RequestSayHelloBidir(&context, &stream, cq, cq,
																	Base::RequestTag());
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
		std::cout << std::this_thread::get_id()
							<< " finished: " << status.error_code() << " "
							<< status.error_details() << std::endl;
		delete this;
	}
	void OnDone(bool ok) noexcept override {
		std::cout << std::this_thread::get_id() << " done" << std::endl;
		if (context.IsCancelled())
			Finish(Status::CANCELLED);
	}

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
					static_cast<HandlerBase *>(tag)->Proceed(ok);
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
	}
};

int main() {
	std::string server_address("0.0.0.0:50051");
	ServerImpl server(server_address);
	server.Run();
	return 0;
}