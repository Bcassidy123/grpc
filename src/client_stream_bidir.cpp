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

template <typename W, typename R, bool Threadsafe = true>
class ClientAsyncReaderWriter {
protected:
	virtual ~ClientAsyncReaderWriter() {}
	void Init(grpc::ClientAsyncReaderWriter<W, R> *stream) noexcept {
		this->stream = stream;
		num_in_flight = 1;
		stream->StartCall(&creator);
	}
	void ReadInitialMetadata() noexcept {
		++num_in_flight;
		stream->ReadInitialMetadata(&initial_metadata_reader);
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
	void WritesDone() noexcept {
		++num_in_flight;
		stream->WritesDone(&writes_doner);
	}
	void Finish(Status *status) noexcept {
		++num_in_flight;
		stream->Finish(status, &finisher);
	}

private:
	virtual void OnCreate(bool ok) noexcept {}
	virtual void OnReadInitialMetadata(bool ok) noexcept {}
	virtual void OnRead(bool ok) noexcept {}
	virtual void OnWrite(bool ok) noexcept {}
	virtual void OnWritesDone(bool ok) noexcept {}
	virtual void OnFinish(bool ok) noexcept {}
	virtual void OnFinally() noexcept {}

private:
	void Finally() noexcept {
		if (finished && num_in_flight == 0) {
			OnFinally();
		}
	}

private:
	grpc::ClientAsyncReaderWriter<W, R> *stream;
	std::conditional_t<Threadsafe, std::atomic_bool, bool> finished;
	std::conditional_t<Threadsafe, std::atomic_uint, unsigned> num_in_flight;
	Handler creator = [this](bool ok) noexcept {
		OnCreate(ok);
		--num_in_flight;
		Finally();
	};
	Handler initial_metadata_reader = [this](bool ok) noexcept {
		OnReadInitialMetadata(ok);
		--num_in_flight;
		Finally();
	};
	Handler reader = [this](bool ok) noexcept {
		OnRead(ok);
		--num_in_flight;
		Finally();
	};
	Handler writer = [this](bool ok) noexcept {
		OnWrite(ok);
		--num_in_flight;
		Finally();
	};
	Handler writes_doner = [this](bool ok) noexcept {
		OnWritesDone(ok);
		--num_in_flight;
		Finally();
	};
	Handler finisher{[this](bool ok) noexcept {
		OnFinish(ok);
		finished = true;
		--num_in_flight;
		Finally();
	}};
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