#pragma once

#include <string>
#include <utility>

namespace meeting {
namespace common {  

// 定义状态码枚举, gRPC
enum class StatusCode {
    kOk = 0,
    kInvalidArgument = 3,
    kNotFound = 5,
    kAlreadyExists = 6,
    kUnauthenticated = 16,
    kInternal = 13,
    kUnavailable = 14,
};

// 表示操作结果的状态
class Status {
public:
    Status() = default;
    Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message)) {}
    
    static Status OK() {
        return Status(StatusCode::kOk, "");
    }
    static Status InvalidArgument(std::string message) {
        return Status(StatusCode::kInvalidArgument, std::move(message));
    }
    static Status NotFound(std::string message) {
        return Status(StatusCode::kNotFound, std::move(message));
    }
    static Status AlreadyExists(std::string message) {
        return Status(StatusCode::kAlreadyExists, std::move(message));
    }
    static Status Unauthenticated(std::string message) {        
        return Status(StatusCode::kUnauthenticated, std::move(message));
    }
    static Status Internal(std::string message) {        
        return Status(StatusCode::kInternal, std::move(message));
    }
    static Status Unavailable(std::string message) {        
        return Status(StatusCode::kUnavailable, std::move(message));
    }

    bool IsOk() const {
        return code_ == StatusCode::kOk;
    }
    StatusCode Code() const {
        return code_;
    }
    const std::string& Message() const {
        return message_;
    }
private:
    StatusCode code_;
    std::string message_;
};

inline std::string StatusCodeToString(StatusCode code) {
    switch (code) {
        case StatusCode::kOk:
            return "OK";
        case StatusCode::kInvalidArgument:
            return "Invalid Argument";
        case StatusCode::kNotFound:
            return "Not Found";
        case StatusCode::kAlreadyExists:
            return "Already Exists";
        case StatusCode::kUnauthenticated:
            return "Unauthenticated";
        case StatusCode::kInternal:
            return "Internal";
        case StatusCode::kUnavailable:
            return "Unavailable";
        default:
            return "Unknown";
    }
}

}
}