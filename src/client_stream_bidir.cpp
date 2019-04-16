#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>

#include "helloworld.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

class ClientBase {
public:
	virtual ~ClientBase() {}
	virtual void Proceed(bool ok) = 0;
	virtual void Write(HelloRequest) {}
};

class ClientData : public ClientBase {
	enum State { CREATE, PROCESS, FINISH };

public:
	ClientData(std::shared_ptr<Channel> channel, grpc::CompletionQueue *cq)
			: reader(*this), writer(*this) {
		stub_ = Greeter::NewStub(channel);
		stream = stub_->PrepareAsyncSayHelloBidir(&context, cq);
		stream->StartCall(this);
	}

	void Proceed(bool ok) override {
		if (state == CREATE) {
			reader.Read();
			state = PROCESS;
		} else if (state == PROCESS) {
		} else {
			delete this;
		}
	}
	void Write(HelloRequest req) override { writer.Write(req); }

private:
	void Finish() {
		if (read_completed && write_completed) {
			stream->Finish(&status, this);
			state = FINISH;
		}
	}
	class Reader : public ClientBase {
		ClientData &parent;
		HelloReply reply;

	public:
		Reader(ClientData &parent) : parent(parent) {}
		void Read() { parent.stream->Read(&reply, this); }
		void Proceed(bool ok) override {
			if (ok) {
				// do something with request here
				std::cout << "read: " << reply.message() << std::endl;
				// foo(reply)
				parent.stream->Read(&reply, this);
			} else {
				parent.read_completed = true;
				parent.Finish();
			}
		}
	};
	class Writer : public ClientBase {
		bool write_in_progress = false;
		ClientData &parent;
		std::list<HelloRequest> queue;

	public:
		Writer(ClientData &parent) : parent(parent) {}
		void Write(HelloRequest request) {
			queue.emplace_back(std::move(request));
			if (!write_in_progress) {
				parent.stream->Write(queue.front(), this);
				write_in_progress = true;
			}
		}
		void Proceed(bool ok) override {
			if (ok) {
				write_in_progress = false;
				[[maybe_unused]] auto request = queue.front();
				// do something with request here
				std::cout << "write: " << request.name() << std::endl;
				// foo(request)
				queue.pop_front();
				if (!queue.empty()) {
					parent.stream->Write(queue.front(), this);
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
	ClientContext context;
	std::unique_ptr<Greeter::Stub> stub_;
	std::unique_ptr<grpc::ClientAsyncReaderWriter<HelloRequest, HelloReply>>
			stream;

	Reader reader;
	Writer writer;
	Status status;
	bool read_completed = false;
	bool write_completed = false;
};

int main() {
	std::string server_address("localhost:50051");

	auto channel =
			grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
	CompletionQueue cq;
	void *tag;
	bool ok = false;
	bool once = false;
	new ClientData(channel, &cq);
	while (cq.Next(&tag, &ok)) {
		static_cast<ClientBase *>(tag)->Proceed(ok);
		if (ok && !once) {
			once = true;
			HelloRequest req;
			req.set_name("John doe");
			static_cast<ClientBase *>(tag)->Write(req);
		}
	}

	return 0;
}