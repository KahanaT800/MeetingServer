#pragma once

#include "common/status.hpp"

#include <utility>

namespace meeting {
namespace common {

// 返回一个包含状态或值的对象
template <typename T>
class StatusOr {
public:
    StatusOr(const Status& status) : status_(status) {}
    StatusOr(Status&& status) : status_(std::move(status)) {}

    template <class U = T, std::enable_if_t<std::is_constructible_v<T, U&&>, int> = 0>
    explicit StatusOr(U&& value)
        : status_(Status::OK()), value_(std::forward<U>(value)) {}

    bool IsOk() const {
        return status_.IsOk();
    }
    const Status& GetStatus() const {
        return status_;
    }

    // 访问存储的值
    T& Value() & {
        return value_;
    }
    T&& Value() && {
        return std::move(value_);
    }
    const T& Value() const& {
        return value_;
    }
    const T&& Value() const&& = delete;
private:
    Status status_;
    T value_{};
};

}
}