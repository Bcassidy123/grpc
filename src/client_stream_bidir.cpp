#include <atomic>
#include <chrono>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
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

template <typename W, typename R> class ClientAsyncReaderWriter {
protected:
	ClientAsyncReaderWriter(
			grpc::ClientAsyncReaderWriter<W, R> *stream = nullptr) noexcept
			: stream(stream) {}
	virtual ~ClientAsyncReaderWriter() {}
	void Init(grpc::ClientAsyncReaderWriter<W, R> *stream) noexcept {
		this->stream = stream;
	}
	void ReadInitialMetadata() noexcept {
		stream->ReadInitialMetadata(&initial_metadata_reader);
	}
	void Read(R *r) noexcept { stream->Read(r, &reader); }
	void Write(W const &w) noexcept { stream->Write(w, &writer); }
	void Write(W const &w, grpc::WriteOptions const &opt) noexcept {
		stream->Write(w, opt, &writer);
	}
	void WritesDone() noexcept { stream->WritesDone(&writes_doner); }
	void Finish(Status *status) noexcept { stream->Finish(status, &finisher); }

	void *StartTag() noexcept { return &creator; }

private:
	virtual void OnCreate(bool ok) noexcept {}
	virtual void OnReadInitialMetadata(bool ok) noexcept {}
	virtual void OnRead(bool ok) noexcept {}
	virtual void OnWrite(bool ok) noexcept {}
	virtual void OnWritesDone(bool ok) noexcept {}
	virtual void OnFinish(bool ok) noexcept {}

private:
	grpc::ClientAsyncReaderWriter<W, R> *stream;
	Handler creator = [this](bool ok) noexcept { OnCreate(ok); };
	Handler initial_metadata_reader = [this](bool ok) noexcept {
		OnReadInitialMetadata(ok);
	};
	Handler reader = [this](bool ok) noexcept { OnRead(ok); };
	Handler writer = [this](bool ok) noexcept { OnWrite(ok); };
	Handler writes_doner = [this](bool ok) noexcept { OnWritesDone(ok); };
	Handler finisher{[this](bool ok) noexcept { OnFinish(ok); }};
};
class CallData : private ClientAsyncReaderWriter<HelloRequest, HelloReply> {

	using Base = ClientAsyncReaderWriter<HelloRequest, HelloReply>;

	std::unique_ptr<grpc::ClientAsyncReaderWriter<HelloRequest, HelloReply>>
			stream;
	grpc::ClientContext context;
	grpc::Status status;

public:
	CallData(std::shared_ptr<Channel> channel, grpc::CompletionQueue *cq) {
		auto stub = Greeter::NewStub(channel);
		stream = stub->PrepareAsyncSayHelloBidir(&context, cq);
		Base::Init(stream.get());
		stream->StartCall(StartTag());
	}
	void Write(std::string something) {}
	void Finish() {}

private: // overrides
	void OnCreate(bool ok) noexcept override {
		if (ok) {
			std::cout << std::this_thread::get_id() << " create" << std::endl;
			ReadInitialMetadata();
		} else {
			std::cout << std::this_thread::get_id() << " create error" << std::endl;
		}
	}
	void OnReadInitialMetadata(bool ok) noexcept override {
		if (ok) {
			std::cout << std::this_thread::get_id() << " read metadata" << std::endl;
			Base::Finish(&status);
		} else {
			std::cout << std::this_thread::get_id() << " read metadata error"
								<< std::endl;
		}
	}
	void OnRead(bool ok) noexcept override {
		if (ok) {
			std::cout << std::this_thread::get_id() << " read" << std::endl;
			//	std::cout << std::this_thread::get_id() << " read: " <<
			// reply.message()
			//			<< std::endl;
		} else {
			std::cout << std::this_thread::get_id() << " read error " << std::endl;
		}
	}
	void OnWrite(bool ok) noexcept override {
		if (ok) {
			std::cout << std::this_thread::get_id() << " write" << std::endl;
			// std::cout << std::this_thread::get_id()
			//						<< " write: " << pending_writes.front().name() << std::endl;
		} else {
			std::cout << std::this_thread::get_id() << " write error " << std::endl;
			WritesDone();
		}
	}
	void OnWritesDone(bool ok) noexcept override {
		if (ok) {
			std::cout << std::this_thread::get_id() << " write done " << std::endl;
		} else {
			std::cout << std::this_thread::get_id() << " write done error"
								<< std::endl;
		}
	}
	void OnFinish(bool ok) noexcept override {
		std::cout << std::this_thread::get_id() << " finish" << std::endl;
		std::cout << std::this_thread::get_id()
							<< " status: " << status.error_code() << " "
							<< status.error_details() << std::endl;
		// delete this;
	}
};

int main() {
	std::string server_address("localhost:50051");

	auto channel =
			grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
	CompletionQueue cq;
	auto c = new CallData(channel, &cq);
	auto f = [&]() {
		void *tag;
		bool ok = false;
		while (cq.Next(&tag, &ok)) {
			std::cout << "ok: " << ok << std::endl;
			static_cast<Handler *>(tag)->Proceed(ok);
		}
	};
	void *tag;
	bool ok = false;
	while (cq.Next(&tag, &ok)) {
		std::cout << "ok: " << ok << std::endl;
		static_cast<Handler *>(tag)->Proceed(ok);
	}
	/*
	std::thread t1(f);
	std::thread t2(f);
	std::thread t3(f);
	std::thread t4(f);
	std::string j;
	while (std::cin >> j) {
		if (j == "end") {
			//	c->Finish();
			break;
		}
		//	c->Write(j);
	}
	*/
	// f();
	/*
	shutdown = true;
	cq.Shutdown();
	t1.join();
	t2.join();
	t3.join();
	t4.join();
	*/
	std::cout << " Good" << std::endl;

	return 0;
}