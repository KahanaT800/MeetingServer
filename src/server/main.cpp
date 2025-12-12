#include "common/config_loader.hpp"
#include "common/logger.hpp"
#include "core/meeting/meeting_manager.hpp"
#include "server/meeting_service_impl.hpp"
#include "server/user_service_impl.hpp"

#include <cstdlib>
#include <csignal>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include <thread>

namespace {

volatile std::sig_atomic_t g_stop_signal = 0;

void HandleSignal(int signal) {
    g_stop_signal = signal;
}

} // namespace

int main(int argc, char** argv) {
    std::string config_path;
    if (argc > 1) {
        config_path = argv[1];
    } else if (const char* env = std::getenv("MEETING_SERVER_CONFIG")) {
        config_path = env;
    } else {
        config_path = meeting::common::GetConfigPath("app.example.json");
    }

    meeting::common::AppConfig config;
    try {
        config = meeting::common::ConfigLoader::Load(config_path);
    } catch (const std::exception& ex) {
        fprintf(stderr, "Failed to load config %s: %s\n", config_path.c_str(), ex.what());
        return EXIT_FAILURE;
    }

    meeting::common::InitLogger(config.logging);
    MEETING_LOG_INFO("Meeting server starting with config {}", config_path);

    meeting::server::UserServiceImpl user_service(config.thread_pool.config_path);
    meeting::server::MeetingServiceImpl meeting_service(config.thread_pool.config_path);

    grpc::ServerBuilder builder;
    std::string address = config.server.host + ":" + std::to_string(config.server.port);
    builder.AddListeningPort(address, grpc::InsecureServerCredentials());
    builder.RegisterService(&user_service);
    builder.RegisterService(&meeting_service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        MEETING_LOG_ERROR("Failed to start gRPC server on {}", address);
        return EXIT_FAILURE;
    }

    MEETING_LOG_INFO("Meeting server listening on {}", address);

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    std::thread shutdown_thread([&server]() {
        while (g_stop_signal == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        MEETING_LOG_WARN("Signal {} received, shutting down gRPC server...", g_stop_signal);
        server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
    });

    server->Wait();
    shutdown_thread.join();
    meeting::common::ShutdownLogger();
    return EXIT_SUCCESS;
}
