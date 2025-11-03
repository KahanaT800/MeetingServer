#include "logger.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/async_logger.h>
#include <spdlog/common.h>
#include <spdlog/details/thread_pool.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/daily_file_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using nlohmann::json;
namespace thread_pool::log {
namespace {
// Sink specification
struct SinkSpec {
    std::string type{"console"};             // sink type
    bool        enabled{true};                // whether enabled
    std::string pattern;                     // override pattern for this sink
    std::string level;                       // override level for this sink
    std::string file_path;                   // file path for file-based sinks
    std::size_t max_size{10 * 1024 * 1024};  // max bytes for rotating file sink
    std::size_t max_files{5};                // number of rotating files to keep
    bool        rotate_on_open{false};       // rotate on open
    int         rotation_hour{0};            // rotation hour
    int         rotation_minute{0};          // rotation minute
    bool        truncate{false};             // whether to truncate
};

struct LoggerConfig {
    std::string               name{"mpmc-threadpool"};                                                 // logger name
    spdlog::level::level_enum level{spdlog::level::info};                                              // log level
    spdlog::level::level_enum flush_level{spdlog::level::warn};                                        // flush-on level
    std::string               pattern{"[%Y-%m-%d %H:%M:%S.%e][%^%l%$][%t] %v"};                        // output pattern
    bool                      async{true};                                                             // whether async
    std::size_t               queue_size{8192};                                                        // async queue size
    std::size_t               workers{std::max<std::size_t>(1, std::thread::hardware_concurrency())};  // async worker threads
    bool                      enable_backtrace{false};                                                 // enable backtrace
    std::size_t               backtrace_depth{0};                                                      // backtrace depth
    std::vector<SinkSpec>     sinks;                                                                   // sink list
};

LoggerPtr g_logger; // global logger
std::mutex g_logger_mtx;
std::shared_ptr<spdlog::details::thread_pool> g_thread_pool; // spdlog thread pool

constexpr const char* kConfigPath = "config/logger_config.json";

// Utilities
// Case-insensitive helper
std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

// Parsing utilities
spdlog::level::level_enum SafeParseLevel(const std::string& level, spdlog::level::level_enum fallback) noexcept {
    try {
        return spdlog::level::from_str(level);
    } catch (const spdlog::spdlog_ex&) {
        return fallback;
    }
}

// --- Input layer ---

// Default config
json DefaultConfig() {
    return json{
        {"logger",
            {
                {"name", "mpmc-threadpool"},
                {"level", "info"},
                {"flush_level", "warn"},
                {"pattern", "[%Y-%m-%d %H:%M:%S.%e][%^%l%$][%t] %v"},
                {"async", true},
                {"async_queue_size", 8192},
                {"async_workers", 1},
                {"enable_backtrace", false},
                {"backtrace_depth", 0}
            }
        },
        {"sinks",
            json::array({
                {
                    {"type", "console"},
                    {"enabled", true},
                    {"pattern", ""},
                    {"level", ""}
                },
                {
                    {"type", "rotating_file"},
                    {"file_path", "logs/pool.log"},
                    {"max_size", 10485760},
                    {"max_files", 5},
                    {"rotate_on_open", false},
                    {"pattern", ""},
                    {"level", ""}
                }
            })
        }
    };
};

// Load config from a file
json LoadConfigFromFile(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error("thread_pool_log: cannot open logger config file: " + path);
    }
    json jcfg;
    ifs >> jcfg;
    return jcfg;
}

json LoadConfig() {
    try {
        return LoadConfigFromFile(kConfigPath);
    } catch (...) {
        return DefaultConfig();
    }
}

// Parse sink specification
SinkSpec ParseSink(const json& jcfg) {
    SinkSpec spec;
    spec.type = ToLower(jcfg.value("type", std::string("console")));
    spec.enabled = jcfg.value("enabled", true);
    spec.pattern = jcfg.value("pattern", std::string{});
    spec.level = ToLower(jcfg.value("level", std::string{}));
    spec.file_path = jcfg.value("file_path", std::string{});
    spec.max_size = jcfg.value("max_size", spec.max_size);
    spec.max_files = jcfg.value("max_files", spec.max_files);
    spec.rotate_on_open = jcfg.value("rotate_on_open", spec.rotate_on_open);
    spec.rotation_hour = jcfg.value("rotation_hour", spec.rotation_hour);
    spec.rotation_minute = jcfg.value("rotation_minute", spec.rotation_minute);
    spec.truncate = jcfg.value("truncate", spec.truncate);
    return spec;
}

// Parse top-level LoggerConfig
LoggerConfig ParseConfig(const json& jcfg) {
    LoggerConfig cfg;
    cfg.name = jcfg.value("name", cfg.name);
    cfg.pattern = jcfg.value("pattern", cfg.pattern);
    cfg.level = SafeParseLevel(ToLower(jcfg.value("level", std::string{"info"})), cfg.level);
    cfg.flush_level = SafeParseLevel(ToLower(jcfg.value("flush_level", std::string{"warn"})), cfg.flush_level);
    cfg.async = jcfg.value("async", cfg.async);
    cfg.queue_size = jcfg.value("async_queue_size", cfg.queue_size);
    cfg.workers = jcfg.value("async_workers", cfg.workers);
    cfg.enable_backtrace = jcfg.value("enable_backtrace", cfg.enable_backtrace);
    cfg.backtrace_depth = jcfg.value("backtrace_depth", cfg.backtrace_depth);

    if (cfg.workers == 0) {
        cfg.workers = 1;
    }
    if (cfg.queue_size == 0) {
        cfg.queue_size = 1024;
    }

    if (auto sinks_it = jcfg.find("sinks"); sinks_it != jcfg.end() && sinks_it->is_array()) {
        cfg.sinks.reserve(sinks_it->size());
        for (const auto& sink_json : *sinks_it) {
            cfg.sinks.push_back(ParseSink(sink_json));
        }
    }

    // Fallback sinks if none configured
    if (cfg.sinks.empty()) {
        cfg.sinks.emplace_back(ParseSink(json::object({{"type", "console"}})));
        cfg.sinks.emplace_back(ParseSink(json::object({
            {"type", "rotating_file"},
            {"file_path", "logs/pool.log"},
            {"max_size", 10485760},
            {"max_files", 5}
        })));
    }
    return cfg;
}

// --- Construction layer ---

// Build console sink
spdlog::sink_ptr BuildConsoleSink(const SinkSpec& spec) {
    if (spec.type == "stderr" || spec.type == "stderr_color") {
        return std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    }
    return std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
}

spdlog::sink_ptr BuildRotateSink(const SinkSpec& spec) {
    auto path = std::filesystem::path(spec.file_path.empty() ? "logs/pool.log" : spec.file_path);
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    return std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        path.string(), spec.max_size, spec.max_files, spec.rotate_on_open);
}

spdlog::sink_ptr BuildDailySink(const SinkSpec& spec) {
    auto path = std::filesystem::path(spec.file_path.empty() ? "logs/pool.log" : spec.file_path);
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }

    const int hour = std::clamp(spec.rotation_hour, 0, 23);
    const int minute = std::clamp(spec.rotation_minute, 0, 59);
    return std::make_shared<spdlog::sinks::daily_file_sink_mt>(
        path.string(), hour, minute, spec.truncate);
}

spdlog::sink_ptr BuildBasicFileSink(const SinkSpec& spec) {
    auto path = std::filesystem::path(spec.file_path.empty() ? "logs/pool.log" : spec.file_path);
    auto parent = path.parent_path(); 
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    return std::make_shared<spdlog::sinks::basic_file_sink_mt>(path.string(), spec.truncate);
}

// Dispatch to specific sink builder based on type
spdlog::sink_ptr BuildSink(const SinkSpec& spec) {
    if (!spec.enabled) {
        return {};
    }
    if (spec.type == "console" || spec.type == "stdout" || spec.type == "stdout_color" ||
        spec.type == "stderr"  || spec.type == "stderr_color") {
        return BuildConsoleSink(spec);
    }
    if (spec.type == "rotating" || spec.type == "rotating_file") {
        return BuildRotateSink(spec);
    }
    if (spec.type == "daily" || spec.type == "daily_file") {
        return BuildDailySink(spec);
    }
    if (spec.type == "file" || spec.type == "basic_file") {
        return BuildBasicFileSink(spec);
    }
    return {};
}

// Apply per-sink overrides
void ApplySinkOverrides(const SinkSpec& spec, const spdlog::sink_ptr& sink) {
    if (!sink) {
        return;
    }
    if (!spec.pattern.empty()) {
        sink->set_pattern(spec.pattern);
    }
    if (!spec.level.empty()) {
        sink->set_level(SafeParseLevel(spec.level, sink->level()));
    }
}

// Register logger
void RegisterLogger(const LoggerPtr& logger) {
    if (!logger) {
        return;
    }
    spdlog::drop(logger->name());
    spdlog::register_logger(logger);
    logger->set_error_handler([](const std::string& msg) {
        std::fputs(msg.c_str(), stderr);
        std::fputc('\n', stderr);
    });
}

// Build and register logger
LoggerPtr BuildLogger(const LoggerConfig& cfg) {
    std::vector<spdlog::sink_ptr> sinks;
    sinks.reserve(cfg.sinks.size());
    for (const auto& spec : cfg.sinks) {
        if (auto sink = BuildSink(spec)) {
            ApplySinkOverrides(spec, sink);
            sinks.emplace_back(std::move(sink));
        }
    }
    if (sinks.empty()) {
        sinks.emplace_back(BuildConsoleSink(SinkSpec{}));
    }

    LoggerPtr logger;
    if (cfg.async) {
        // Create a dedicated spdlog thread pool
        auto pool = std::make_shared<spdlog::details::thread_pool>(cfg.queue_size, cfg.workers);
        logger = std::make_shared<spdlog::async_logger>(
            cfg.name, sinks.begin(), sinks.end(), pool, spdlog::async_overflow_policy::block);
        g_thread_pool = std::move(pool);
    } else {
        logger = std::make_shared<spdlog::logger>(cfg.name, sinks.begin(), sinks.end());
        g_thread_pool.reset();
    }

    logger->set_level(cfg.level);
    logger->set_pattern(cfg.pattern);
    logger->flush_on(cfg.flush_level);

    if (cfg.enable_backtrace) {
        const auto depth = cfg.backtrace_depth == 0 ? 32 : cfg.backtrace_depth;
        logger->enable_backtrace(depth);
    }

    RegisterLogger(logger);
    return logger;
}

// Initialization
LoggerPtr EnsureInitialized() {
    if (auto current = std::atomic_load_explicit(&g_logger, std::memory_order_acquire)) {
        return current;
    }
    std::lock_guard<std::mutex> lk(g_logger_mtx);
    if (auto current = std::atomic_load_explicit(&g_logger, std::memory_order_relaxed)) {
        return current;
    }
    auto jcfg = LoadConfig();
    auto cfg = ParseConfig(jcfg);
    auto logger = BuildLogger(cfg);
    std::atomic_store_explicit(&g_logger, logger, std::memory_order_release);
    return logger;
}

// Store new logger instance
void StoreLogger(const LoggerPtr& logger) {
    std::atomic_store_explicit(&g_logger, logger, std::memory_order_release);
}

}

// --- Output layer ---
spdlog::level::level_enum detail::ParseLevel(std::string_view level) noexcept {
    return SafeParseLevel(std::string(level), spdlog::level::info);
}

LoggerPtr LoadLogger() {
    return EnsureInitialized();
}

void SetLogger(LoggerPtr logger) {
    if (!logger) {
        return;
    }
    std::lock_guard<std::mutex> lk(g_logger_mtx);
    RegisterLogger(logger);
    StoreLogger(std::move(logger));
}

bool LoggerIsReady() noexcept {
    return std::atomic_load_explicit(&g_logger, std::memory_order_acquire) != nullptr;
}

LoggerPtr InitDefault() {
    auto jcfg = DefaultConfig();
    return InitFromJson(jcfg);
}

LoggerPtr InitFromFile(const std::string& path) {
    auto jcfg = LoadConfigFromFile(path);
    return InitFromJson(jcfg);
}

LoggerPtr InitFromJson(const json& jcfg) {
    std::lock_guard<std::mutex> lk(g_logger_mtx);

    auto cfg = ParseConfig(jcfg);
    auto logger = BuildLogger(cfg);
    StoreLogger(logger);
    return logger;
}

void SetLevel(const std::string& level) {
    auto logger = LoadLogger();
    if (logger) {
        logger->set_level(SafeParseLevel(ToLower(level), logger->level()));
    }
}

spdlog::level::level_enum Level() {
    if (auto logger = std::atomic_load_explicit(&g_logger, std::memory_order_acquire)) {
        return logger->level();
    }
    return spdlog::level::off;
}

void InitializeLogger(const std::string& path) noexcept {
    if (LoggerIsReady()) {
        return;
    }

    std::lock_guard<std::mutex> lk(g_logger_mtx);
    if (std::atomic_load_explicit(&g_logger, std::memory_order_relaxed)) {
        return;
    }

    try {
        auto jcfg = LoadConfigFromFile(path);
        auto cfg = ParseConfig(jcfg);
        auto logger = BuildLogger(cfg);
        StoreLogger(logger);
        return;
    } catch (...) {}

    auto jcfg = DefaultConfig();
    auto cfg = ParseConfig(jcfg);
    auto logger = BuildLogger(cfg);
    StoreLogger(logger);
}
}