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

template <typename W, typename R>
class ServerAsyncReaderWriter : public CallBase {
	using Parent = ServerAsyncReaderWriter;
	enum State { CREATE, PROCESS, FINISH };

protected:
	ServerAsyncReaderWriter(grpc::ServerContext *context) noexcept
			: stream(context), context(context) {}
	void Proceed(bool ok) noexcept override {
		if (state == CREATE) {
			if (ok && !context->IsCancelled()) {
				reader = std::make_unique<Reader>(*this);
				writer = std::make_unique<Writer>(*this);
				OnCreate();
				state = PROCESS;
			} else {
				Finish({grpc::StatusCode::UNKNOWN, "could not create"});
			}
		} else if (state == PROCESS) {
			if (ok && !context->IsCancelled()) {
				OnSendMetadata();
			} else {
				Finish({grpc::StatusCode::UNKNOWN, "could not send metadata"});
			}
		} else {
			OnFinish();
		}
	}

	void SendInitialMetadata() noexcept { stream.SendInitialMetadata(this); }
	void Read() noexcept { reader->Read(); }
	void Write(W w) noexcept { writer->Write(std::move(w)); }
	void Write(W, grpc::WriteOptions) noexcept {}
	void WriteAndFinish(W const &w, grpc::WriteOptions const &opt,
											grpc::Status const &status) noexcept {
		writer->WriteAndFinish(w, opt, status);
	}
	void Finish(grpc::Status const &status) noexcept {
		state = FINISH;
		stream.Finish(status, this);
	}

private:
	virtual void OnCreate() noexcept {}
	virtual void OnSendMetadata() noexcept {}
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
		void Read() { parent.stream.Read(&read, this); }
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
		bool write_and_finish = false;

	public:
		Writer(Parent &parent) : parent(parent) {}
		void Proceed(bool ok) override {
			if (ok) {
				parent.OnWrite(write);
				if (write_and_finish) {
					parent.OnWriteDone();
				}
			} else {
				parent.OnWriteDone();
			}
			if (write_and_finish) {
				auto &p = parent;
				p.writer.reset();
				p.state = FINISH;
				p.Proceed(ok);
			}
		}
		void Write(W w) {
			write = std::move(w);
			parent.stream.Write(write, this);
		}
		void Write(W w, grpc::WriteOptions opt) noexcept {
			write = std::move(w);
			parent.stream.Write(write, opt, this);
		}
		void WriteAndFinish(W w, grpc::WriteOptions opt,
												grpc::Status const &status) noexcept {
			write_and_finish = true;
			write = std::move(w);
			parent.stream.WriteAndFinish(write, opt, status, this);
		}
	};

protected:
	grpc::ServerAsyncReaderWriter<W, R> stream;
	grpc::ServerContext *context;

private:
	State state = CREATE;
	std::unique_ptr<Reader> reader;
	std::unique_ptr<Writer> writer;
};

class CallData final
		: public ServerAsyncReaderWriter<HelloReply, HelloRequest> {
	using Base = ServerAsyncReaderWriter<HelloReply, HelloRequest>;

public:
	CallData(helloworld::Greeter::AsyncService *service,
					 grpc::ServerCompletionQueue *cq)
			: Base(&context), service(service), cq(cq) {
		context.AsyncNotifyWhenDone(this);
		service->RequestSayHelloBidir(&context, &stream, cq, cq, this);
	}

private:
	void OnCreate() noexcept override {
		std::cout << std::this_thread::get_id() << " created" << std::endl;
		new CallData(service, cq);
		SendInitialMetadata();
	}
	void OnSendMetadata() noexcept override {
		std::cout << std::this_thread::get_id() << " sent metadata " << std::endl;
		Read();
	}
	void OnRead(HelloRequest const &request) noexcept override {
		std::cout << std::this_thread::get_id() << " read: " << request.name()
							<< std::endl;
		HelloReply reply;
		reply.set_message("You sent: " + request.name());
		Read();
		std::lock_guard l{pending_writes_mutex};
		pending_writes.push_back(reply);
		if (pending_writes.size() == 1) {
			Write(pending_writes.front());
		}
	}
	void OnReadDone() noexcept override {
		std::cout << std::this_thread::get_id() << " read done" << std::endl;
	}
	void OnWrite(HelloReply const &reply) noexcept override {
		std::cout << std::this_thread::get_id() << " wrote: " << reply.message()
							<< std::endl;
		std::lock_guard l{pending_writes_mutex};
		pending_writes.pop_front();
		if (!pending_writes.empty()) {
			Write(pending_writes.front());
		}
	}
	void OnWriteDone() noexcept override {
		std::cout << std::this_thread::get_id() << " write done" << std::endl;
	}
	void OnFinish() noexcept override {
		std::cout << std::this_thread::get_id() << " finished" << std::endl;
		delete this;
	}

private:
	helloworld::Greeter::AsyncService *service;
	grpc::ServerCompletionQueue *cq;
	grpc::ServerContext context;
	grpc::Status status;
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