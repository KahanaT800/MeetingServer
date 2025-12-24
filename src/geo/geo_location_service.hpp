#pragma once

#include "common/status.hpp"
#include "common/status_or.hpp"

#include <string>
#include <cstdint>
#include <maxminddb.h>

namespace meeting{
namespace geo{

struct GeoInfo {
    std::string country;
    std::string region;
    std::string city;
    std::string iso_code;
    std::string timezone;
    double latitude = 0.0;
    double longitude = 0.0;
    bool is_private = false;
};

// GeoLite2 封装类
class GeoLocationService {
public:
    explicit GeoLocationService(const std::string& db_path);
    ~GeoLocationService();

    // 根据 IP 获取地理位置信息
    meeting::common::StatusOr<GeoInfo> Lookup(const std::string& ip) const;

    const std::string& DbPath() const {
        return db_path_;
    }

    bool IsAvailable() const {
        return db_available_;
    }
private:
    // 检查是否为私有 IP 地址
    bool IsPrivateIpv4(std::uint32_t addr) const;
    bool IsPrivateIpv6(const unsigned char* addr) const;
private:
    std::string db_path_; // 数据库路径
    bool db_available_ = false; // 数据库是否可用
    bool opened_ = false; // 数据库是否已打开
    MMDB_s mmdb_{}; // MaxMind 数据库句柄
};

} // namespace geo    
} // namespace meeting