#include <chrono>
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
	virtual void Finish() {}
};

class ClientData : public ClientBase {
	enum State { CREATE, PROCESS, FINISH };

public:
	ClientData(std::shared_ptr<Channel> channel, grpc::CompletionQueue *cq)
			: reader(*this), writer(*this) {
		stub_ = Greeter::NewStub(channel);
		stream = stub_->PrepareAsyncSayHelloBidir(&context, cq);
		stream->StartCall(this);
		stream->ReadInitialMetadata(this);
	}

	void Proceed(bool ok) override {
		if (state == CREATE) {
			reader.Read();
			state = PROCESS;
		} else if (state == PROCESS) {
		} else {
			std::cout << status.error_code() << std::endl;
		}
	}
	void Write(HelloRequest req) override { writer.Write(req); }

	void Finish() override {
		context.TryCancel();
		stream->Finish(&status, this);
		state = FINISH;
	}

private:
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
				Finish();
			}
		}
		void Finish() override {
			parent.read_completed = true;
			if (parent.write_completed) {
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
		void Write(HelloRequest request) override {
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
				Finish();
			}
		}
		void Finish() override {
			parent.write_completed = true;
			parent.stream->WritesDone(this);
			if (parent.read_completed) {
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
	auto c = new ClientData(channel, &cq);

	int i = 0;
	bool finish_once = false;
	while (true) {
		auto nextStatus = cq.AsyncNext(
				&tag, &ok, std::chrono::system_clock::now() + std::chrono::seconds(2));
		if (nextStatus == grpc::CompletionQueue::NextStatus::GOT_EVENT) {
			static_cast<ClientBase *>(tag)->Proceed(ok);
			if (ok && !once) {
				once = true;
				HelloRequest req;
				req.set_name("John doe");
				static_cast<ClientBase *>(tag)->Write(req);
			}

		} else if (nextStatus == grpc::CompletionQueue::NextStatus::TIMEOUT) {
			++i;
			if (!finish_once) {
				finish_once = true;
				c->Finish();
			} else {
				cq.Shutdown();
			}
		} else {
			break;
		}
	}
	delete c;
	return 0;
}