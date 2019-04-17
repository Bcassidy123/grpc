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

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class CallBase {
public:
	virtual ~CallBase() {}
	virtual void Proceed(bool ok) = 0;
};

struct Handler : public CallBase {
	std::function<void(bool)> func;
	Handler() = default;
	template <typename F, typename = std::enable_if_t<!std::is_base_of_v<
														Handler, std::remove_reference_t<F>>>>
	Handler(F &&f) : func(f) {}
	void Proceed(bool ok) override { func(ok); }
};

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
	void *CancelTag() noexcept { return &canceller; }

private:
	virtual void OnCreate() noexcept {}
	virtual void OnCreateError() noexcept {}
	virtual void OnSendInitialMetadata() noexcept {}
	virtual void OnSendInitialMetadataError() noexcept {}
	virtual void OnRead() noexcept {}
	virtual void OnReadDone() noexcept {}
	virtual void OnWrite() noexcept {}
	virtual void OnWriteDone() noexcept {}
	virtual void OnFinish() noexcept {}
	virtual void OnCancelled() noexcept {}

private:
	grpc::ServerAsyncReaderWriter<W, R> *stream;
	grpc::ServerContext *context;
	Handler creator{[this](bool ok) {
		if (ok)
			OnCreate();
		else
			OnCreateError();
	}};
	Handler initial_metadata_sender{[this](bool ok) {
		if (ok)
			OnSendInitialMetadata();
		else
			OnSendInitialMetadataError();
	}};
	Handler reader{[this](bool ok) {
		if (ok)
			OnRead();
		else
			OnReadDone();
	}};
	Handler writer{[this](bool ok) {
		if (ok)
			OnWrite();
		else
			OnWriteDone();
	}};
	Handler write_and_finisher{[this](bool ok) {
		if (ok) {
			OnWrite();
		}
		OnWriteDone();
		OnFinish();
	}};
	Handler finisher{[this](bool) { OnFinish(); }};
	Handler canceller{[this](bool) { OnCancelled(); }};
};

class CallData final
		: public ServerAsyncReaderWriter<HelloReply, HelloRequest> {
	using Base = ServerAsyncReaderWriter<HelloReply, HelloRequest>;

public:
	CallData(helloworld::Greeter::AsyncService *service,
					 grpc::ServerCompletionQueue *cq)
			: Base(&stream, &context), service(service), cq(cq), stream(&context) {
		context.AsyncNotifyWhenDone(Base::CancelTag());
		service->RequestSayHelloBidir(&context, &stream, cq, cq,
																	Base::RequestTag());
	}

private:
	void OnCreate() noexcept override {
		std::cout << std::this_thread::get_id() << " created" << std::endl;
		new CallData(service, cq);
		SendInitialMetadata();
	}
	void OnCreateError() noexcept override {
		std::cout << std::this_thread::get_id() << " created error" << std::endl;
		new CallData(service, cq);
		Finish({grpc::StatusCode::INTERNAL, "Something went wrong"});
	}
	void OnSendInitialMetadata() noexcept override {
		std::cout << std::this_thread::get_id() << " sent metadata " << std::endl;
		Read(&request);
	}
	void OnSendInitialMetadataError() noexcept override {
		std::cout << std::this_thread::get_id() << " send metadata error"
							<< std::endl;
		Finish({grpc::StatusCode::INTERNAL, "Something went wrong"});
	}
	void OnRead() noexcept override {
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
	}
	void OnReadDone() noexcept override {
		std::cout << std::this_thread::get_id() << " read done" << std::endl;
	}
	void OnWrite() noexcept override {
		std::lock_guard l{pending_writes_mutex};
		std::cout << std::this_thread::get_id()
							<< " wrote: " << pending_writes.front().message() << std::endl;
		pending_writes.pop_front();
		if (!pending_writes.empty()) {
			Write(pending_writes.front());
		}
	}
	void OnWriteDone() noexcept override {
		std::cout << std::this_thread::get_id() << " write done" << std::endl;
	}
	void OnFinish() noexcept override {
		std::cout << std::this_thread::get_id()
							<< " finished: " << status.error_code() << " "
							<< status.error_details() << std::endl;
		delete this;
	}
	void OnCancelled() noexcept override {
		std::cout << std::this_thread::get_id() << " cancelled" << std::endl;
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
					static_cast<CallBase *>(tag)->Proceed(ok);
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