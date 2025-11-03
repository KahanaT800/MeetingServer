#pragma once

#include "thread_pool/fwd.hpp"

#include <nlohmann/json.hpp>
#include <spdlog/logger.h>
#include <memory>
#include <string>
#include <string_view>
#include <chrono>
#include <functional>

namespace thread_pool::log {
// External initialization APIs
void InitializeLogger(const std::string& path = "config/logger_config.json") noexcept;

// Get/Set global logger
LoggerPtr LoadLogger();
void SetLogger(LoggerPtr lg);
bool LoggerIsReady() noexcept;

// Input layer: initialize logger
LoggerPtr InitDefault();
LoggerPtr InitFromFile(const std::string &file_path);
LoggerPtr InitFromJson(const nlohmann::json &jcfg);

// Log level
void SetLevel(const std::string& level);
spdlog::level::level_enum Level();

namespace detail {
    // Parse level string
    spdlog::level::level_enum ParseLevel(std::string_view level) noexcept;

    // Logging dispatch template
    template <typename... Args>
    inline void Log(spdlog::level::level_enum level
                    , fmt::format_string<Args...> fmt
                    , Args&&... args) {
        auto lg = ::thread_pool::log::LoadLogger(); 
        if (lg && lg->should_log(level)) {
            lg->log(level, fmt, std::forward<Args>(args)...);
        }
    }
}

// Scope timer
class ScopeTimer {
public:
    using Clock = std::chrono::steady_clock;
    using Hook = std::function<void(std::chrono::nanoseconds)>; // callback

    explicit ScopeTimer(std::string name, Hook hook = {}
                        , spdlog::level::level_enum level = spdlog::level::debug)
        : name_(std::move(name)), hook_(std::move(hook)), level_(level), start_(Clock::now()) {}

    ~ScopeTimer() noexcept {
        const auto span = Clock::now() - start_;
        if (hook_) {
            hook_(std::chrono::duration_cast<std::chrono::nanoseconds>(span));
        }
        auto lg = ::thread_pool::log::LoadLogger();
        if (lg && lg->should_log(level_)) {
            lg->log(level_, "[perf] {} took {} us",
                    name_, std::chrono::duration_cast<std::chrono::microseconds>(span).count());
        }
    }
private:
    std::string name_;
    Hook hook_;
    spdlog::level::level_enum level_;
    Clock::time_point start_;
};
}

// Logging macros
#define TP_LOG_TRACE(...) ::thread_pool::log::detail::Log(spdlog::level::trace, __VA_ARGS__)
#define TP_LOG_DEBUG(...) ::thread_pool::log::detail::Log(spdlog::level::debug, __VA_ARGS__)
#define TP_LOG_INFO(...) ::thread_pool::log::detail::Log(spdlog::level::info,  __VA_ARGS__)
#define TP_LOG_WARN(...) ::thread_pool::log::detail::Log(spdlog::level::warn,  __VA_ARGS__)
#define TP_LOG_ERROR(...) ::thread_pool::log::detail::Log(spdlog::level::err,   __VA_ARGS__)
#define TP_LOG_CRITICAL(...) ::thread_pool::log::detail::Log(spdlog::level::critical, __VA_ARGS__)
// Conditional logging macro
#define TP_LOG_IF(cond, level, ...) do { if (cond) { TP_LOG_##LEVEL(__VA_ARGS__); } } while(0)

// Performance logging macros
#define TP_CONCAT_IMPL(a,b) a##b
#define TP_CONCAT(a,b) TP_CONCAT_IMPL(a,b)

#define TP_PERF_SCOPE(name) ::thread_pool::log::ScopeTimer TP_CONCAT(_scope_timer_, __LINE__)(name)
#define TP_PERF_SCOPE_LEVEL(name, level) ::thread_pool::log::ScopeTimer TP_CONCAT(__scope_timer_, __LINE__)(name, {}, level)
#define TP_PERF_SCOPE_HOOK(name, hook) ::thread_pool::log::ScopeTimer TP_CONCAT(__scope_timer_, __LINE__)(name, hook)
#define TP_PERF_SCOPE_HOOK_LEVEL(name, hook, level) ::thread_pool::log::ScopeTimer TP_CONCAT(__scope_timer_, __LINE__)(name, hook, level)