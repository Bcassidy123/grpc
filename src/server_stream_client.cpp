#include <iostream>
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
	enum CallStatus { CREATE, PROCESS, FINISH };
	CallStatus status = CREATE;

public:
	virtual ~CallBase() {}
	void Proceed(bool ok) {
		if (status == CREATE) {
			Create();
			status = PROCESS;
		} else if (status == PROCESS) {
			status = Process(ok) ? PROCESS : FINISH;
		} else {
			delete this;
		}
	}

private:
	virtual void Create() = 0;
	virtual bool Process(bool ok) = 0;
};

class CallData : public CallBase {
	helloworld::Greeter::AsyncService *service;
	grpc::ServerCompletionQueue *cq;
	ServerContext context;
	grpc::ServerAsyncReader<HelloReply, HelloRequest> request;
	grpc::ServerAsyncResponseWriter<HelloReply> responder;
	HelloReply reply;
	HelloRequest helloRequest;
	std::string all = "hello ";
	int i = 0;

public:
	CallData(helloworld::Greeter::AsyncService *service,
					 grpc::ServerCompletionQueue *cq)
			: service(service), cq(cq), request(&context), responder(&context) {
		Proceed(true);
	}

private:
	void Create() override {
		service->RequestSayHellosClient(&context, &request, cq, cq, this);
	}
	bool Process(bool ok) override {
		if (i++ == 0) {
			new CallData(service, cq);
			request.Read(&helloRequest, this);
		} else {
			if (ok) {
				all += helloRequest.name();
				std::cout << "Got: " << helloRequest.name() << std::endl;
				request.Read(&helloRequest, this);
				return true;
			} else {
				reply.set_message(all);
				request.Finish(reply, Status::OK, this);
				std::cout << "Finish " << std::endl;
				return false;
			}
		}
	}
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
		while (true) {
			cq->Next(&tag, &ok);
			std::cout << ok << std::endl;
			static_cast<CallData *>(tag)->Proceed(ok);
		}
	}
};

int main() {
	std::string server_address("0.0.0.0:50051");
	ServerImpl server(server_address);
	server.Run();
	return 0;
}