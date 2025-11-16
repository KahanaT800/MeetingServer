#include "common/config_loader.hpp"
#include "common/logger.hpp"
#include "thread_pool/include/logger.hpp"

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace {
std::filesystem::path TempPath(const std::string& suffix) {
    auto base = std::filesystem::temp_directory_path();
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    return base / ("meeting_server_test_" + suffix + "_" + std::to_string(now));
}
} // namespace

class ConfigLoaderTest : public ::testing::Test {
protected:
    void TearDown() override {
        if (!temp_file_.empty()) {
            std::error_code ec;
            std::filesystem::remove(temp_file_, ec);
        }
    }

    std::filesystem::path WriteTempConfig(const std::string& content) {
        temp_file_ = TempPath("config.json");
        std::ofstream ofs(temp_file_);
        ofs << content;
        ofs.flush();
        return temp_file_;
    }

private:
    std::filesystem::path temp_file_;
};

TEST_F(ConfigLoaderTest, LoadsLoggingConfig) {
    const std::string config_json = R"({
        "logging": {
            "level": "debug",
            "pattern": "[%H:%M:%S] %v",
            "console": false,
            "file": "temp/logs/server.log",
            "integrate_thread_pool_logger": true
        },
        "thread_pool": {
            "config_path": "custom/thread_pool.json"
        }
    })";
    auto config_path = WriteTempConfig(config_json);

    auto cfg = meeting::common::ConfigLoader::Load(config_path.string());
    EXPECT_EQ(cfg.logging.level, "debug");
    EXPECT_EQ(cfg.logging.pattern, "[%H:%M:%S] %v");
    EXPECT_FALSE(cfg.logging.console);
    EXPECT_EQ(cfg.logging.file, "temp/logs/server.log");
    EXPECT_TRUE(cfg.logging.integrate_thread_pool_logger);
    EXPECT_EQ(cfg.thread_pool.config_path, "custom/thread_pool.json");
}

class LoggerInitTest : public ::testing::Test {
protected:
    void TearDown() override {
        meeting::common::ShutdownLogger();
        if (!temp_dir_.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(temp_dir_, ec);
        }
    }

    std::filesystem::path PrepareLogPath(const std::string& filename) {
        temp_dir_ = TempPath("logs");
        return temp_dir_ / "logs" / filename;
    }

    std::filesystem::path temp_dir_;
};

TEST_F(LoggerInitTest, CreatesDirectories) {
    auto log_file = PrepareLogPath("meeting.log");

    meeting::common::LoggingConfig config;
    config.console = false;
    config.level = "warn";
    config.pattern = "[test] %v";
    config.file = log_file.string();
    config.integrate_thread_pool_logger = true;

    meeting::common::InitLogger(config);

    auto logger = meeting::common::GetLogger();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->level(), spdlog::level::warn);
    EXPECT_TRUE(std::filesystem::exists(log_file.parent_path()));

    // 触发一次日志写入，确保文件被创建
    MEETING_LOG_WARN("logger integration test");

    EXPECT_TRUE(std::filesystem::exists(log_file));
    EXPECT_EQ(logger, thread_pool::log::LoadLogger());
}

TEST_F(LoggerInitTest, InvalidLevelo) {
    meeting::common::LoggingConfig config;
    config.console = false;
    config.level = "not-a-level";

    meeting::common::InitLogger(config);

    auto logger = meeting::common::GetLogger();
    ASSERT_NE(logger, nullptr);
    EXPECT_EQ(logger->level(), spdlog::level::info);
}
