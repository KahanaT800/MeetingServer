#include <cstdlib>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <exception>
#include <fmt/core.h>
#include <grpcpp/grpcpp.h>
#include <thread>

#include "logger.hpp"
#include "thread_pool/config.hpp"
#include "thread_pool/thread_pool.hpp"
#include "server/health_service.h"

namespace {
std::string resolve_port() {
    if (const char* env = std::getenv("MEETING_SERVER_PORT")) {
        return std::string(env);
    }
    return "0.0.0.0:50051";
}

thread_pool::ThreadPoolConfig load_thread_pool_config(const std::string& path) {
    thread_pool::ThreadPoolConfig cfg;
    try {
        if (std::filesystem::exists(path)) {
            if (auto loader = thread_pool::ThreadPoolConfigLoader::FromFile(path); loader && loader->Ready()) {
                cfg = loader->GetConfig();
                fmt::print("[server] thread pool config loaded from {}\n", path);
            }
        } else {
            fmt::print("[server] thread pool config '{}' not found, using defaults\n", path);
        }
    } catch (const std::exception& ex) {
        fmt::print(stderr, "[server] failed to load thread pool config '{}': {}\n", path, ex.what());
    }
    return cfg;
}

std::atomic<bool> g_shutdown_requested{false};

void signal_handler(int) noexcept {
    g_shutdown_requested.store(true, std::memory_order_release);
}

void install_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifdef SIGQUIT
    std::signal(SIGQUIT, signal_handler);
#endif
}
}  // namespace

int main() {
    thread_pool::log::InitializeLogger();

    const auto server_address = resolve_port();
    const std::string pool_config_path = "config/thread_pool.json";
    auto pool_config = load_thread_pool_config(pool_config_path);

    thread_pool::ThreadPool pool(pool_config);
    pool.Start();

    meeting::server::HealthServiceImpl health_service(pool);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&health_service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        fmt::print(stderr, "[server] failed to start on {}\n", server_address);
        pool.Stop(thread_pool::StopMode::Force);
        return EXIT_FAILURE;
    }

    g_shutdown_requested.store(false, std::memory_order_release);
    install_signal_handlers();

    std::thread shutdown_watcher([&] {
        while (!g_shutdown_requested.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        fmt::print("\n[server] shutdown initiated\n");
        server->Shutdown();
    });

    fmt::print("[server] listening on {}\n", server_address);
    server->Wait();

    g_shutdown_requested.store(true, std::memory_order_release);
    if (shutdown_watcher.joinable()) {
        shutdown_watcher.join();
    }

    pool.ShutDown(thread_pool::ShutDownOption::Graceful);
    return EXIT_SUCCESS;
}
