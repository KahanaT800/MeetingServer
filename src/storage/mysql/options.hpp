#pragma once

#include <string>
#include <cstdint>
#include <chrono>

namespace meeting {
namespace storage {

struct Options {
    std::string host = "127.0.0.1";
    std::uint16_t port = 3306;
    std::string user = "dev";
    std::string password;
    std::string database = "meeting";
    std::size_t pool_size = 4;
    std::chrono::milliseconds acquire_timeout{500};
    std::chrono::milliseconds connect_timeout{500};
    std::chrono::milliseconds read_timeout{2000};
    std::chrono::milliseconds write_timeout{2000};
    std::string charset = "utf8mb4";
};

}
}