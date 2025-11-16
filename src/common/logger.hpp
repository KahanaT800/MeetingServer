#pragma once

#include "common/config.hpp"

#include <spdlog/logger.h>
#include <memory>

namespace meeting {
namespace common {

void InitLogger(const LoggingConfig& config);
void ShutdownLogger();
std::shared_ptr<spdlog::logger> GetLogger();

#define MEETING_LOG_INFO(...)  ::meeting::common::GetLogger()->info(__VA_ARGS__)
#define MEETING_LOG_WARN(...)  ::meeting::common::GetLogger()->warn(__VA_ARGS__)
#define MEETING_LOG_ERROR(...) ::meeting::common::GetLogger()->error(__VA_ARGS__)

}
}