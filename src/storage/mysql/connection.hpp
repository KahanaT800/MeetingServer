#pragma once

#include "common/status.hpp"
#include "common/status_or.hpp"
#include "storage/mysql/options.hpp"

#include <mysql/mysql.h>
#include <memory>

namespace meeting {
namespace storage {

class Connection {
public:
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    static meeting::common::StatusOr<std::unique_ptr<Connection>> Create(const Options& options);

    MYSQL* Raw() const noexcept {return handle_;}
    const Options& GetOptions() const noexcept {return options_;}
private:
    Connection(MYSQL* handle, Options options);

    MYSQL* handle_ = nullptr;
    Options options_;
};

}
}