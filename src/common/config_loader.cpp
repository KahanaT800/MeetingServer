#include "common/config_loader.hpp"

#include "config_path.hpp"
#include "common/logger.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace meeting {
namespace common {

namespace {
AppConfig g_config;
bool g_config_initialized = false; // 全局配置初始化标志

// 检测配置文件路径
std::string DetectConfigPath() {
     if (const char* env = std::getenv("MEETING_SERVER_CONFIG")) {
        return env;
    }
    return GetConfigPath("app.example.json");
}

} // namespace

AppConfig ConfigLoader::Load(const std::string& path) {
    auto json = ReadFile(path);
    return FromJson(json);
}

AppConfig ConfigLoader::LoadFromEnvOrDefault() {
    return Load(DetectConfigPath());
}

const AppConfig& GlobalConfig() {
    if (!g_config_initialized) {
        g_config = ConfigLoader::LoadFromEnvOrDefault();
        g_config_initialized = true;
    }
    return g_config;
}

nlohmann::json ConfigLoader::ReadFile(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        throw std::runtime_error("Failed to open config file: " + path);
    }
    return nlohmann::json::parse(ifs, nullptr, true, true);
}

// 从JSON对象构建配置结构体
AppConfig ConfigLoader::FromJson(const nlohmann::json& j) {
    AppConfig cfg;
    // 解析各个配置部分
    // Server配置
    if (j.contains("server")) {
        const auto& server = j["server"];
        cfg.server.host = server.value("host", cfg.server.host);
        cfg.server.port = server.value("port", cfg.server.port);
    }
    // Logging配置
    if (j.contains("logging")) {
        const auto& logging = j["logging"];
        cfg.logging.level = logging.value("level", cfg.logging.level);
        cfg.logging.pattern = logging.value("pattern", cfg.logging.pattern);
        cfg.logging.console = logging.value("console", cfg.logging.console);
        cfg.logging.file = logging.value("file", cfg.logging.file);
        cfg.logging.integrate_thread_pool_logger =
            logging.value("integrate_thread_pool_logger", cfg.logging.integrate_thread_pool_logger);
    }
    // ThreadPool配置
    if (j.contains("thread_pool")) {
        cfg.thread_pool.config_path = j["thread_pool"].value("config_path", cfg.thread_pool.config_path);
    }
    // GeoIP配置
    if (j.contains("geoip")) {
        cfg.geoip.db_path = j["geoip"].value("db_path", cfg.geoip.db_path);
    }
    // Zookeeper配置
    if (j.contains("zookeeper")) {
        cfg.zookeeper.hosts = j["zookeeper"].value("hosts", cfg.zookeeper.hosts);
    }
    // Storage配置
    if (j.contains("storage")) {
        const auto& storage = j["storage"];
        if (storage.contains("mysql")) {
            const auto& mysql = storage["mysql"];
            cfg.storage.mysql.host = mysql.value("host", cfg.storage.mysql.host);
            cfg.storage.mysql.port = mysql.value("port", cfg.storage.mysql.port);
            cfg.storage.mysql.user = mysql.value("user", cfg.storage.mysql.user);
            cfg.storage.mysql.password = mysql.value("password", cfg.storage.mysql.password);
            cfg.storage.mysql.database = mysql.value("database", cfg.storage.mysql.database);
            cfg.storage.mysql.pool_size = mysql.value("pool_size", cfg.storage.mysql.pool_size);
            cfg.storage.mysql.connection_timeout_ms = mysql.value("connection_timeout_ms", cfg.storage.mysql.connection_timeout_ms);
            cfg.storage.mysql.read_timeout_ms = mysql.value("read_timeout_ms", cfg.storage.mysql.read_timeout_ms);
            cfg.storage.mysql.write_timeout_ms = mysql.value("write_timeout_ms", cfg.storage.mysql.write_timeout_ms);
            cfg.storage.mysql.enabled = mysql.value("enabled", cfg.storage.mysql.enabled);
        }
    }
    return cfg;
}

}
}