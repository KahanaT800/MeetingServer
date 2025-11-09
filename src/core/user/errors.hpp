#pragma once

#include "common.pb.h"
#include "common/status.hpp"
#include "common/status_or.hpp"

#include <string>

namespace meeting {
namespace core {

enum class UserErrorCode {
    kOk = 0,
    kUserNameExists = 1,
    kInvalidPassword = 2,
    kInvalidCredentials = 3,
    kSessionExpired = 4,
    kUserNotFound = 5,
};

// 将 UserErrorCode 转换为通用 Status
inline ::meeting::common::Status FromUserError(UserErrorCode error, std::string message = "") {
    using ::meeting::common::Status;
    switch (error) {
        case UserErrorCode::kOk:
            return Status::OK();
        case UserErrorCode::kUserNameExists:
            return Status::AlreadyExists(message.empty() ? "User name already exists" : message);
        case UserErrorCode::kInvalidPassword:
            return Status::InvalidArgument(message.empty() ? "Invalid password" : message);
        case UserErrorCode::kInvalidCredentials:
            return Status::Unauthenticated(message.empty() ? "Invalid credentials" : message);
        case UserErrorCode::kSessionExpired:
            return Status::Unauthenticated(message.empty() ? "Session expired" : message);  
        case UserErrorCode::kUserNotFound:
            return Status::NotFound(message.empty() ? "User not found" : message);
    }
    return Status::Internal("Unknown user error");
}

// 将错误信息填充到 protobuf Error 消息中
inline void ErrorToProto(UserErrorCode error, const ::meeting::common::Status& status
                        , ::proto::common::Error* error_proto) {
    if (!error_proto) {
        return;
    }
    error_proto->set_code(static_cast<int32_t>(error));
    error_proto->set_message(status.Message());
}


}
}