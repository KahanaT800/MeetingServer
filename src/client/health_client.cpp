#include <iostream>
#include <memory>
#include <string>

#include <grpcpp/grpcpp.h>
#include "health.grpc.pb.h"
#include "health.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using meeting::health::HealthService;
using meeting::health::HealthCheckRequest;
using meeting::health::HealthCheckResponse;

class HealthClient {
public:
    HealthClient(std::shared_ptr<Channel> channel)
        : stub_(HealthService::NewStub(channel)) {}

    std::string Check(const std::string& source) {
        HealthCheckRequest request;
        request.set_source(source);

        HealthCheckResponse response;
        ClientContext context;

        Status status = stub_->Check(&context, request, &response);

        if (status.ok()) {
            return "Status: " + response.status() + ", Timestamp: " + std::to_string(response.timestamp());
        } else {
            return "RPC failed: " + status.error_message();
        }
    }

private:
    std::unique_ptr<HealthService::Stub> stub_;
};

int main(int argc, char** argv) {
    std::string server_address("localhost:50051");
    if (argc > 1) {
        server_address = argv[1];
    }

    auto channel = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    HealthClient client(channel);

    std::cout << "Testing health service at " << server_address << std::endl;
    
    std::string source = "test-client";
    std::string result = client.Check(source);
    std::cout << "Health check result: " << result << std::endl;

    return 0;
}