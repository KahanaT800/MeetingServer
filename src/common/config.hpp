#pragma once

#include <string>
#include <string_view>

namespace meeting {
namespace common {

// 服务器配置结构体
struct ServerConfig {
    std::string host = "0.0.0.0";
    int port = 50051;
};

// 日志配置结构体
struct LoggingConfig {
    std::string level = "info";
    std::string pattern = "[%Y-%m-%d %H:%M:%S.%e][%^%l%$][%t] %v";
    bool console = true;
    std::string file = "";
    bool integrate_thread_pool_logger = false;
};

// 线程池配置路径结构体
struct ThreadPoolConfigPath {
    std::string config_path = "config/thread_pool.json";
};

// GeoIP配置结构体
struct GeoIPConfig {
    std::string db_path = "";
};

// Zookeeper配置结构体
struct ZookeeperConfig {
    std::string hosts = "127.0.0.1:2181";
};

// Mysql配置结构体
struct MysqlConfig {
    std::string host = "127.0.0.1";
    int port = 3306;
    std::string user = "dev";
    std::string password = "";
    std::string database = "meeting";
    int pool_size = 4;
    int connection_timeout_ms = 500;
    int read_timeout_ms = 2000;
    int write_timeout_ms = 2000;
    bool enabled = false;
};

// 存储配置结构体
struct StorageConfig {
    MysqlConfig mysql;
};

// 应用配置结构体
struct AppConfig {
    ServerConfig server;
    LoggingConfig logging;
    ThreadPoolConfigPath thread_pool;
    GeoIPConfig geoip;
    ZookeeperConfig zookeeper;
    StorageConfig storage;
};

}
}