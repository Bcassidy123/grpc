#include <cassert>
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

template <typename C, typename W, typename R>
class ServerAsyncReaderWriter : public CallBase {
	using Parent = ServerAsyncReaderWriter;
	enum State { CREATE, PROCESS, FINISH };

public:
	ServerAsyncReaderWriter() noexcept : stream(&context) {}
	void Proceed(bool ok) noexcept override {
		if (state == CREATE) {
			reader = std::make_unique<Reader>(*this);
			writer = std::make_unique<Writer>(*this);
			OnCreate();
			state = PROCESS;
		} else if (state == PROCESS) {
			OnSendMetadata();
		} else {
			OnFinish();
		}
	}
	void SendInitialMetadata() noexcept { stream.SendInitialMetadata(this); }
	void Read() noexcept {
		if (reader)
			reader->Read();
	}
	void Write(W w) noexcept {
		if (writer)
			writer->Write(std::move(w));
	}
	void Write(W, grpc::WriteOptions) noexcept {}
	void WriteAndFinish(W const &w, grpc::WriteOptions const &opt,
											grpc::Status const &status) noexcept {
		if (writer)
			writer->WriteAndFinish(w, opt, status);
		else
			Finish(status);
	}
	void Finish(grpc::Status const &status) noexcept {
		if (writer) {
			writer->Finish(status);
		} else {
			state = FINISH;
			stream.Finish(status, this);
		}
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
		bool write_in_progress = false;
		Parent &parent;
		std::list<W> pending_writes;
		bool should_finish = false;
		grpc::Status finish_status;

	public:
		Writer(Parent &parent) : parent(parent) {}
		void Proceed(bool ok) override {
			if (ok) {
				assert(pending_writes.empty() == false);
				write_in_progress = false;
				parent.OnWrite(pending_writes.front());
				pending_writes.pop_front();
				if (!pending_writes.empty()) {
					parent.stream.Write(pending_writes.front(), this);
					write_in_progress = true;
				} else if (should_finish) {
					auto status = finish_status;
					auto &p = parent;
					parent.writer.reset();
					p.OnWriteDone();
					p.Finish(status);
				}
			} else {
				auto &p = parent;
				parent.writer.reset();
				p.OnWriteDone();
			}
		}
		void Write(W w) {
			pending_writes.emplace_back(std::move(w));
			if (!write_in_progress) {
				parent.stream.Write(pending_writes.front(), this);
				write_in_progress = true;
			}
		}
		void WriteAndFinish(W w, grpc::WriteOptions opt,
												grpc::Status const &status) noexcept {
			pending_writes.emplace_back(std::move(w));
			should_finish = true;
			finish_status = status;
			if (!write_in_progress) {
				parent.stream.WriteAndFinish(pending_writes.front(), opt, status, this);
				write_in_progress = true;
			}
		}
		void Finish(grpc::Status const &status) noexcept {
			if (write_in_progress) {
				should_finish = true;
				finish_status = status;
			} else {
				auto &p = parent;
				parent.writer.reset();
				p.OnWriteDone();
				p.Finish(status);
			}
		}
	};

protected:
	grpc::ServerAsyncReaderWriter<W, R> stream;
	grpc::ServerContext context;

private:
	State state = CREATE;
	std::unique_ptr<Reader> reader;
	std::unique_ptr<Writer> writer;
};

class CallData final
		: public ServerAsyncReaderWriter<CallData, HelloReply, HelloRequest> {
	using Base = ServerAsyncReaderWriter<CallData, HelloReply, HelloRequest>;

	int i = 0;

public:
	CallData(helloworld::Greeter::AsyncService *service,
					 grpc::ServerCompletionQueue *cq)
			: service(service), cq(cq) {
		service->RequestSayHelloBidir(&context, &stream, cq, cq, this);
	}

private:
	void OnCreate() noexcept override {
		std::cout << "created" << std::endl;
		new CallData(service, cq);
		SendInitialMetadata();
	}
	void OnSendMetadata() noexcept override {
		std::cout << "sent metadata " << std::endl;
		Read();
	}
	void OnRead(HelloRequest const &request) noexcept override {
		std::cout << "read: " << request.name() << std::endl;
		HelloReply reply;
		reply.set_message("You sent: " + request.name());
		if (i++ < 3) {
			Read();
			Write(reply);
		} else {
			WriteAndFinish(reply, {}, status);
		}
	}
	void OnReadDone() noexcept override {
		std::cout << "read done" << std::endl;
		Finish(status);
	}
	void OnWrite(HelloReply const &reply) noexcept override {
		std::cout << "wrote: " << reply.message() << std::endl;
	}
	void OnWriteDone() noexcept override {
		std::cout << "write done" << std::endl;
	}
	void OnFinish() noexcept override {
		delete this;
		std::cout << "finished" << std::endl;
	}

private:
	helloworld::Greeter::AsyncService *service;
	grpc::ServerCompletionQueue *cq;
	grpc::Status status;
	bool read_done;
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