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

using grpc::Channel;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class CallBase {
public:
	virtual ~CallBase() {}
	virtual void Proceed(bool ok) = 0;
};

template <typename W, typename R>
class ClientAsyncReaderWriter : public CallBase {
	using Parent = ClientAsyncReaderWriter;
	enum State { CREATE, PROCESS, FINISH };

protected:
	ClientAsyncReaderWriter(
			grpc::ClientAsyncReaderWriter<W, R> *stream = nullptr) noexcept
			: stream(stream) {}
	void Proceed(bool ok) noexcept override {
		if (state == CREATE) {
			if (ok) {
				reader = std::make_unique<Reader>(*this);
				writer = std::make_unique<Writer>(*this);
				OnCreate();
				state = PROCESS;
			} else {
				Finish();
			}
		} else if (state == PROCESS) {
			if (ok) {
				OnReadInitialMetadata();
			} else {
				Finish();
			}
		} else {
			OnFinish();
		}
	}

	void ReadInitialMetadata() noexcept { stream->ReadInitialMetadata(this); }
	void Read() noexcept { reader->Read(); }
	void Write(W w) noexcept { writer->Write(std::move(w)); }
	void Write(W w, grpc::WriteOptions const &opt) noexcept {
		writer->Write(std::move(w), opt);
	}
	void WritesDone() noexcept { writer->WritesDone(); }
	void Finish() noexcept {
		state = FINISH;
		stream->Finish(&status, this);
	}

private:
	virtual void OnCreate() noexcept {}
	virtual void OnReadInitialMetadata() noexcept {}
	virtual void OnRead(R const &) noexcept {}
	virtual void OnReadDone() noexcept {}
	virtual void OnWrite(W const &) noexcept {}
	virtual void OnWriteDone() noexcept {}
	virtual void OnFinish() noexcept {}

private:
	class Reader : public CallBase {
		Parent &parent;
		R read;

	public:
		Reader(Parent &parent) : parent(parent) {}
		void Read() { parent.stream->Read(&read, this); }
		void Proceed(bool ok) override {
			if (ok) {
				parent.OnRead(read);
			} else {
				parent.OnReadDone();
				parent.reader.reset();
			}
		}
	};
	class Writer : public CallBase {
		Parent &parent;
		W write;

	public:
		Writer(Parent &parent) : parent(parent) {}
		void Proceed(bool ok) override {
			if (ok) {
				parent.OnWrite(write);
			} else {
				WritesDone();
			}
		}
		void Write(W w) {
			write = std::move(w);
			parent.stream->Write(write, this);
		}
		void Write(W w, grpc::WriteOptions opt) noexcept {
			write = std::move(w);
			parent.stream->Write(write, opt, this);
		}
		void WritesDone() noexcept {
			parent.OnWriteDone();
			parent.writer.reset();
		}
	};

protected:
	grpc::ClientAsyncReaderWriter<W, R> *stream;
	grpc::Status status;

private:
	State state = CREATE;
	std::unique_ptr<Reader> reader;
	std::unique_ptr<Writer> writer;
};
class CallData : public ClientAsyncReaderWriter<HelloRequest, HelloReply> {

	using Base = ClientAsyncReaderWriter<HelloRequest, HelloReply>;

	std::unique_ptr<grpc::ClientAsyncReaderWriter<HelloRequest, HelloReply>>
			stream;
	grpc::ClientContext context;
	std::list<HelloRequest> pending_writes;
	std::mutex write_mutex;

public:
	CallData(std::shared_ptr<Channel> channel, grpc::CompletionQueue *cq)
			: Base(nullptr) {
		auto stub = Greeter::NewStub(channel);
		stream = stub->PrepareAsyncSayHelloBidir(&context, cq);
		Base::stream = stream.get();
		stream->StartCall(this);
	}
	void OnCreate() noexcept override {
		std::cout << std::this_thread::get_id() << " create" << std::endl;
		ReadInitialMetadata();
	}
	void OnReadInitialMetadata() noexcept override {
		std::cout << std::this_thread::get_id() << " read metadata" << std::endl;
		Read();
		Write("Bob Tabor");
	}
	void OnFinish() noexcept override {
		std::cout << std::this_thread::get_id() << " finish" << std::endl;
	}
	void OnRead(const HelloReply &reply) noexcept override {
		std::cout << std::this_thread::get_id() << " read: " << reply.message()
							<< std::endl;
		if (!quit) {
			Read();
		}
	}
	void OnReadDone() noexcept override {
		std::cout << std::this_thread::get_id() << " read done " << std::endl;
	}
	void OnWrite(const HelloRequest &request) noexcept override {
		std::cout << std::this_thread::get_id() << " write: " << request.name()
							<< std::endl;
		std::lock_guard l{write_mutex};
		pending_writes.pop_front();
		if (!pending_writes.empty()) {
			Base::Write(pending_writes.front());
		}
	}
	void OnWriteDone() noexcept override {
		std::cout << std::this_thread::get_id() << " write done " << std::endl;
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
	void Quit() { quit = true; }
	std::atomic_bool quit = false;
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
			if (!shutdown) {
				static_cast<CallBase *>(tag)->Proceed(ok);
			}
		}
	};
	std::thread t1(f);
	std::thread t2(f);
	std::thread t3(f);
	std::thread t4(f);
	std::string j;
	while (std::cin >> j) {
		c->Write(j);
		if (j == "end")
			break;
	}
	shutdown = true;
	cq.Shutdown();
	f();
	t1.join();
	t2.join();
	t3.join();
	t4.join();
	std::cout << " Good" << std::endl;

	delete c;
	return 0;
}