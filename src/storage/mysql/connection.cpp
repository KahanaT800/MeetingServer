#include "storage/mysql/connection.hpp"

#include <fmt/format.h>

namespace meeting {
namespace storage {

namespace {

// 创建错误状态, 包含 MySQL 错误信息
meeting::common::Status MakeError(const std::string& context, MYSQL* handle) {
    std::string message = context;
    if (handle != nullptr) {
        message += ": ";
        message += mysql_error(handle);
    }
    return meeting::common::Status::Internal(message);
}

} // namespace

Connection::Connection(MYSQL* handle, Options options): handle_(handle), options_(std::move(options)) {}

Connection::~Connection() {
    if (handle_ != nullptr) {
        mysql_close(handle_);
        handle_ = nullptr;
    }
}


// 静态工厂方法, 创建并初始化 MySQL 连接
meeting::common::StatusOr<std::unique_ptr<Connection>> Connection::Create(const Options& options) {
    MYSQL* handle = mysql_init(nullptr);
    if (handle == nullptr) {
        return MakeError("mysql_init failed", nullptr);
    }

    // 设置连接超时
    unsigned int connect_timeout_sec = static_cast<unsigned int>(options.connect_timeout.count() / 1000);
    mysql_options(handle, MYSQL_OPT_CONNECT_TIMEOUT, &connect_timeout_sec);

    // 设置读取超时
    unsigned int read_timeout_sec = static_cast<unsigned int>(options.read_timeout.count() / 1000);
    mysql_options(handle, MYSQL_OPT_READ_TIMEOUT, &read_timeout_sec);

    // 设置写入超时
    unsigned int write_timeout_sec = static_cast<unsigned int>(options.write_timeout.count() / 1000);
    mysql_options(handle, MYSQL_OPT_WRITE_TIMEOUT, &write_timeout_sec);

    if (!mysql_real_connect(handle,
                           options.host.c_str(),
                           options.user.c_str(),
                           options.password.c_str(),
                           options.database.c_str(),
                           options.port,
                           nullptr,
                           CLIENT_MULTI_STATEMENTS)) {
        meeting::common::Status status = MakeError("mysql_real_connect failed", handle);
        mysql_close(handle);
        return status;
    }

    if (!options.charset.empty()) {
        mysql_set_character_set(handle, options.charset.c_str());
    }

    return meeting::common::StatusOr<std::unique_ptr<Connection>>(std::unique_ptr<Connection>(new Connection(handle, options)));
}

} // namespace storage
} // namespace meeting