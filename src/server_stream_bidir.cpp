#include <functional>
#include <iostream>
#include <list>
#include <memory>
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

class CallData : public CallBase {
	enum State { CREATE, PROCESS, FINISH };

public:
	CallData(helloworld::Greeter::AsyncService *service,
					 grpc::ServerCompletionQueue *cq)
			: service(service), cq(cq), stream(&context), reader(*this),
				writer(*this) {
		service->RequestSayHelloBidir(&context, &stream, cq, cq, this);
	}

	void Proceed(bool ok) override {
		if (state == CREATE) {
			new CallData(service, cq);
			stream.SendInitialMetadata(this);
			reader.Read();
			state = PROCESS;
		} else if (state == PROCESS) {
		} else {
			delete this;
		}
	}

private:
	void Finish() {
		if (read_completed && write_completed) {
			stream.Finish(status, this);
			state = FINISH;
		}
	}
	class Reader : public CallBase {
		CallData &parent;
		HelloRequest request;

	public:
		Reader(CallData &parent) : parent(parent) {}
		void Read() { parent.stream.Read(&request, this); }
		void Proceed(bool ok) override {
			if (ok) {
				// do something with request here
				std::cout << "read: " << request.name() << std::endl;
				HelloReply reply;
				reply.set_message("you sent:" + request.name());
				parent.writer.Write(reply);
				// foo(request)
				parent.stream.Read(&request, this);
			} else {
				parent.read_completed = true;
				parent.Finish();
			}
		}
	};
	class Writer : public CallBase {
		bool write_in_progress = false;
		CallData &parent;
		std::list<HelloReply> queue;

	public:
		Writer(CallData &parent) : parent(parent) {}
		void Write(HelloReply reply) {
			queue.emplace_back(std::move(reply));
			if (!write_in_progress) {
				parent.stream.Write(queue.front(), this);
				write_in_progress = true;
			}
		}
		void Proceed(bool ok) override {
			if (ok) {
				write_in_progress = false;
				[[maybe_unused]] auto reply = queue.front();
				// do something with reply here
				std::cout << "write: " << reply.message() << std::endl;
				// foo(reply)
				queue.pop_front();
				if (!queue.empty()) {
					parent.stream.Write(queue.front(), this);
					write_in_progress = true;
				}
			} else {
				parent.write_completed = true;
				parent.Finish();
			}
		}
	};

private:
	State state = CREATE;
	helloworld::Greeter::AsyncService *service;
	grpc::ServerCompletionQueue *cq;
	ServerContext context;
	grpc::ServerAsyncReaderWriter<HelloReply, HelloRequest> stream;

	Reader reader;
	Writer writer;
	Status status;
	bool read_completed = false;
	bool write_completed = false;
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
		new CallData(&service, cq.get());
		void *tag;
		bool ok;
		while (cq->Next(&tag, &ok)) {
			std::cout << ok << std::endl;
			static_cast<CallBase *>(tag)->Proceed(ok);
		}
	}
};

int main() {
	std::string server_address("0.0.0.0:50051");
	ServerImpl server(server_address);
	server.Run();
	return 0;
}