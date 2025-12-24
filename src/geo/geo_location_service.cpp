#include "geo/geo_location_service.hpp"

#include <arpa/inet.h>
#include <sys/stat.h>

#include <cstring>

namespace meeting{
namespace geo{

// 构造函数，初始化数据库路径并检查可用性
GeoLocationService::GeoLocationService(const std::string& db_path)
    : db_path_(std::move(db_path)) {
    // 尝试打开数据库文件
    struct stat st {};
    if (stat(db_path_.c_str(), &st) == 0) {
        // 打开数据库文件
        int status = MMDB_open(db_path_.c_str(), MMDB_MODE_MMAP, &mmdb_);
        if (status == MMDB_SUCCESS) {
            db_available_ = true;
            opened_ = true;
        }
    }
}

// 析构函数，关闭数据库
GeoLocationService::~GeoLocationService() {
    if (opened_) {
        MMDB_close(&mmdb_);
    }
}

// 检查是否为私有 IPv4 地址
bool GeoLocationService::IsPrivateIpv4(uint32_t addr) const {
    // 输入为网络字节序，需转为主机序再判断
    uint32_t host = ntohl(addr);
    uint8_t a = static_cast<uint8_t>(host >> 24);
    uint8_t b = static_cast<uint8_t>(host >> 16);
    // 10.0.0.0/8
    if (a == 10) {
        return true;
    }
    // 172.16.0.0/12
    if (a == 172 && (b >= 16 && b <= 31)) {
        return true;
    }
    // 192.168.0.0/16
    if (a == 192 && b == 168) {
        return true;
    }
    // 127.0.0.0/8
    if (a == 127) {
        return true;
    }
    return false;
}

// 检查是否为私有 IPv6 地址
bool GeoLocationService::IsPrivateIpv6(const unsigned char* addr) const {
    // ::1 Loopback
    bool is_loopback = true;
    for (int i = 0; i < 15; ++i) {
        if (addr[i] != 0) {
            is_loopback = false;
            break;
        }
    }
    if (is_loopback && addr[15] == 1) return true;

    // fe80::/10 链路本地；fc00::/7 ULA
    return (addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80) ||
           ((addr[0] & 0xfe) == 0xfc);
}

// 根据 IP 获取地理位置信息
meeting::common::StatusOr<GeoInfo> GeoLocationService::Lookup(const std::string& ip) const {
    // IP 校验与私网判断
    if (ip.empty()) {
        return meeting::common::Status::InvalidArgument("ip is empty");
    }

    in_addr ipv4 {};
    in6_addr ipv6 {};
    // ipv4 地址检查
    bool is_v4 = inet_pton(AF_INET, ip.c_str(), &ipv4) == 1;
    // ipv6 地址检查
    bool is_v6 = false;
    // 如果不是 ipv4，再检查 ipv6
    if (!is_v4) {
        is_v6 = inet_pton(AF_INET6, ip.c_str(), &ipv6) == 1;
    }

    // 非法地址返回错误
    if (!is_v4 && !is_v6) {
        return meeting::common::Status::InvalidArgument("invalid ip");
    }

    // 私有地址检查
    if (is_v4 && IsPrivateIpv4(ipv4.s_addr)) {
        GeoInfo info;
        info.is_private = true;
        return meeting::common::StatusOr<GeoInfo>(info);
    }
    if (is_v6 && IsPrivateIpv6(ipv6.s6_addr)) {
        GeoInfo info;
        info.is_private = true;
        return meeting::common::StatusOr<GeoInfo>(info);
    }

    if (!db_available_) {
        return meeting::common::Status::Unavailable("GeoIP database not available");
    }

    int gai_error = 0;
    int mmdb_error = 0;
    // 在数据库中查询 IP 地址
    auto result = MMDB_lookup_string(const_cast<MMDB_s*>(&mmdb_), ip.c_str(), &gai_error, &mmdb_error);
    // 处理查询结果
    if (gai_error != 0) {
        return meeting::common::Status::InvalidArgument("invalid ip format");
    }
    // 数据库查询错误处理
    if (mmdb_error != MMDB_SUCCESS) {
        return meeting::common::Status::Unavailable(MMDB_strerror(mmdb_error));
    }
    // 未找到对应条目
    if (!result.found_entry) {
        return meeting::common::Status::NotFound("ip not found in database");
    }

    // 解析查询结果
    GeoInfo info;
    MMDB_entry_s entry = result.entry;
    MMDB_entry_data_s data;

     // 查询国家 ISO 代码
    if (MMDB_get_value(&entry, &data, "country", "iso_code", NULL) == MMDB_SUCCESS && 
        data.has_data && data.type == MMDB_DATA_TYPE_UTF8_STRING) {
        info.iso_code = std::string(data.utf8_string, data.data_size);
    }

    // 查询国家名称
    if (MMDB_get_value(&entry, &data, "country", "names", "en", NULL) == MMDB_SUCCESS && 
        data.has_data && data.type == MMDB_DATA_TYPE_UTF8_STRING) {
        info.country = std::string(data.utf8_string, data.data_size);
    }

    // 查询地区/州
    if (MMDB_get_value(&entry, &data, "subdivisions", "0", "names", "en", NULL) == MMDB_SUCCESS && 
        data.has_data && data.type == MMDB_DATA_TYPE_UTF8_STRING) {
        info.region = std::string(data.utf8_string, data.data_size);
    }

    // 查询城市
    if (MMDB_get_value(&entry, &data, "city", "names", "en", NULL) == MMDB_SUCCESS && 
        data.has_data && data.type == MMDB_DATA_TYPE_UTF8_STRING) {
        info.city = std::string(data.utf8_string, data.data_size);
    }

    // 查询时区
    if (MMDB_get_value(&entry, &data, "location", "time_zone", NULL) == MMDB_SUCCESS && 
        data.has_data && data.type == MMDB_DATA_TYPE_UTF8_STRING) {
        info.timezone = std::string(data.utf8_string, data.data_size);
    }

    // 查询纬度
    if (MMDB_get_value(&entry, &data, "location", "latitude", NULL) == MMDB_SUCCESS && data.has_data) {
        if (data.type == MMDB_DATA_TYPE_DOUBLE) {
            info.latitude = data.double_value;
        } else if (data.type == MMDB_DATA_TYPE_FLOAT) {
            info.latitude = data.float_value;
        }
    }

    // 查询经度
    if (MMDB_get_value(&entry, &data, "location", "longitude", NULL) == MMDB_SUCCESS && data.has_data) {
        if (data.type == MMDB_DATA_TYPE_DOUBLE) {
            info.longitude = data.double_value;
        } else if (data.type == MMDB_DATA_TYPE_FLOAT) {
            info.longitude = data.float_value;
        }
    }

    return meeting::common::StatusOr<GeoInfo>(info);
}


} // namespace geo
} // namespace meeting