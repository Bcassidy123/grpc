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

class CallData {
	helloworld::Greeter::AsyncService *service;
	grpc::ServerCompletionQueue *cq;
	grpc::ServerAsyncResponseWriter<HelloReply> responder;
	ServerContext context;
	HelloRequest request;
	HelloReply reply;
	enum CallStatus { CREATE, PROCESS, FINISH };
	CallStatus status = CREATE;

public:
	CallData(helloworld::Greeter::AsyncService *service,
					 grpc::ServerCompletionQueue *cq)
			: service(service), cq(cq), responder(&context) {
		Proceed();
	}

	void Proceed() {
		if (status == CREATE) {

			status = PROCESS;

			service->RequestSayHello(&context, &request, &responder, cq, cq, this);
		} else if (status == PROCESS) {
			status = FINISH;

			new CallData(service, cq);
			std::string prefix("hello ");
			reply.set_message(prefix + request.name());
			responder.Finish(reply, Status::OK, this);

		} else {
			delete this;
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
			if (!ok)
				break;
			static_cast<CallData *>(tag)->Proceed();
		}
	}
};
int main() {
	std::string server_address("0.0.0.0:50051");
	ServerImpl server(server_address);
	server.Run();
	return 0;
}