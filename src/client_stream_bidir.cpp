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
	ClientAsyncReaderWriter(grpc::ClientAsyncReaderWriter<W, R> *stream = nullptr,
													grpc::Status *status = nullptr) noexcept
			: stream(stream), status(status) {}
	void Init(grpc::ClientAsyncReaderWriter<W, R> *stream,
						grpc::Status *status) noexcept {
		this->stream = stream;
		this->status = status;
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
	void Finish() noexcept { stream->Finish(status, &finisher); }

	void *StartTag() noexcept { return &creator; }

private:
	virtual void OnCreate() noexcept {}
	virtual void OnCreateError() noexcept {}
	virtual void OnReadInitialMetadata() noexcept {}
	virtual void OnReadInitialMetadataError() noexcept {}
	virtual void OnRead() noexcept {}
	virtual void OnReadDone() noexcept {}
	virtual void OnWrite() noexcept {}
	virtual void OnWriteDone() noexcept {}
	virtual void OnFinish() noexcept {}

private:
	grpc::ClientAsyncReaderWriter<W, R> *stream;
	grpc::Status *status;
	Handler creator = [this](bool ok) noexcept {
		if (ok)
			OnCreate();
		else
			OnCreateError();
	};
	Handler initial_metadata_reader = [this](bool ok) noexcept {
		if (ok)
			OnReadInitialMetadata();
		else
			OnReadInitialMetadataError();
	};
	Handler reader = [this](bool ok) noexcept {
		if (ok)
			OnRead();
		else
			OnReadDone();
	};
	Handler writer = [this](bool ok) noexcept {
		if (ok)
			OnWrite();
		else
			WritesDone();
	};
	Handler writes_doner = [this](bool ok) noexcept { OnWriteDone(); };
	Handler finisher = [this](bool ok) noexcept { OnFinish(); };
};
class CallData : public ClientAsyncReaderWriter<HelloRequest, HelloReply> {

	using Base = ClientAsyncReaderWriter<HelloRequest, HelloReply>;

	std::unique_ptr<grpc::ClientAsyncReaderWriter<HelloRequest, HelloReply>>
			stream;
	grpc::ClientContext context;
	grpc::Status status;
	HelloReply reply;
	std::list<HelloRequest> pending_writes;
	std::mutex write_mutex;

public:
	CallData(std::shared_ptr<Channel> channel, grpc::CompletionQueue *cq) {
		auto stub = Greeter::NewStub(channel);
		stream = stub->PrepareAsyncSayHelloBidir(&context, cq);
		Base::Init(stream.get(), &status);
		stream->StartCall(StartTag());
	}
	void Write(std::string something) {
		std::lock_guard l{write_mutex};
		HelloRequest req;
		req.set_name(something);
		pending_writes.push_back(req);
		if (pending_writes.size() == 1) {
			Base::Write(pending_writes.front());
		}
	}
	void Finish() {
		std::lock_guard l{write_mutex};
		pending_writes.clear();
		context.TryCancel(); // cancel everything
		Base::Finish();
	}

private: // overrides
	void OnCreate() noexcept override {
		std::cout << std::this_thread::get_id() << " create" << std::endl;
		ReadInitialMetadata();
	}
	void OnReadInitialMetadata() noexcept override {
		std::cout << std::this_thread::get_id() << " read metadata" << std::endl;
		Read(&reply);
	}
	void OnFinish() noexcept override {
		std::cout << std::this_thread::get_id() << " finish" << std::endl;
		std::cout << std::this_thread::get_id()
							<< " status: " << status.error_code() << " "
							<< status.error_details() << std::endl;
	}
	void OnRead() noexcept override {
		std::cout << std::this_thread::get_id() << " read: " << reply.message()
							<< std::endl;
		Read(&reply);
	}
	void OnReadDone() noexcept override {
		std::cout << std::this_thread::get_id() << " read done " << std::endl;
	}
	void OnWrite() noexcept override {
		std::lock_guard l{write_mutex};
		std::cout << std::this_thread::get_id()
							<< " write: " << pending_writes.front().name() << std::endl;
		pending_writes.pop_front();
		if (!pending_writes.empty()) {
			Base::Write(pending_writes.front());
		}
	}
	void OnWriteDone() noexcept override {
		std::cout << std::this_thread::get_id() << " write done " << std::endl;
		std::lock_guard l{write_mutex};
		pending_writes.clear();
	}
};

int main() {
	std::string server_address("localhost:50051");

	auto channel =
			grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
	CompletionQueue cq;
	auto c = new CallData(channel, &cq);
	std::atomic_bool shutdown = false;
	auto f = [&]() {
		void *tag;
		bool ok = false;
		while (cq.Next(&tag, &ok)) {
			std::cout << "ok: " << ok << std::endl;
			static_cast<HandlerBase *>(tag)->Proceed(ok);
		}
	};
	std::thread t1(f);
	std::thread t2(f);
	std::thread t3(f);
	std::thread t4(f);
	std::string j;
	while (std::cin >> j) {
		if (j == "end") {
			c->Finish();
			break;
		}
		c->Write(j);
	}
	shutdown = true;
	cq.Shutdown();
	f();
	t1.join();
	t2.join();
	t3.join();
	t4.join();
	delete c;
	std::cout << " Good" << std::endl;

	return 0;
}