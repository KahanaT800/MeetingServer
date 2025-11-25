#pragma once

#include "common/config.hpp"

#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace meeting {
namespace common {

class ConfigLoader {
public:
    static AppConfig Load(const std::string& path);
    static AppConfig LoadFromEnvOrDefault();
private:
    static AppConfig FromJson(const nlohmann::json& j);
    static nlohmann::json ReadFile(const std::string& path);
};

// 获取全局配置单例
const AppConfig& GlobalConfig();

}
}