#include "common/logger.hpp"
#include "thread_pool/include/logger.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cstdio>
#include <filesystem>
#include <system_error>
#include <stdexcept>
#include <algorithm>
#include <array>
#include <string_view>

namespace meeting {
namespace common {

namespace {

std::shared_ptr<spdlog::logger> g_logger;

void LogLevelFallback(const std::string& level,
                      std::string_view reason,
                      spdlog::level::level_enum fallback) noexcept {
    std::fprintf(stderr,
                 "meeting_server logger: invalid level \"%s\" (%s); fallback to %s\n",
                 level.c_str(),
                 std::string(reason).c_str(),
                 spdlog::level::to_string_view(fallback).data());
}

spdlog::level::level_enum SafeParseLevel(
    const std::string& level, spdlog::level::level_enum fallback) noexcept {
    std::string normalized = level;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (normalized == "warning") {
        normalized = "warn";
    } else if (normalized == "error") {
        normalized = "err";
    }

    static constexpr std::array<std::string_view, 7> kValidLevels{
        "trace", "debug", "info", "warn", "err", "critical", "off"};

    auto it = std::find(kValidLevels.begin(), kValidLevels.end(), normalized);
    if (it == kValidLevels.end()) {
        LogLevelFallback(level, "not recognized", fallback);
        return fallback;
    }

    try {
        return spdlog::level::from_str(normalized);
    } catch (const spdlog::spdlog_ex& ex) {
        LogLevelFallback(level, ex.what(), fallback);
        return fallback;
    }
}

void EnsureParentDirectory(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    if (parent.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
        throw std::runtime_error("Failed to create log directory " + parent.string() + ": " + ec.message());
    }
}

} // namespace

void InitLogger(const LoggingConfig& config) {
    std::vector<spdlog::sink_ptr> sinks;
    if (config.console) {
        sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
    }
    if (!config.file.empty()) {
        std::filesystem::path log_path{config.file};
        EnsureParentDirectory(log_path);
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(log_path.string(), true));
    }
    g_logger = std::make_shared<spdlog::logger>("meeting_server", sinks.begin(), sinks.end());
    g_logger->set_level(SafeParseLevel(config.level, spdlog::level::info));
    g_logger->set_pattern(config.pattern);
    spdlog::set_default_logger(g_logger);

    if (config.integrate_thread_pool_logger) {
        thread_pool::log::SetLogger(g_logger);
    }
}

void ShutdownLogger() {
    if (g_logger) {
        g_logger->flush();
    }
    spdlog::shutdown();
}

std::shared_ptr<spdlog::logger> GetLogger() {
    if (!g_logger) {
        g_logger = spdlog::default_logger();
    }
    return g_logger;
}

}
}