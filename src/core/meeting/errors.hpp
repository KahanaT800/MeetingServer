#pragma once

#include "common/status.hpp"
#include "meeting_service.pb.h"

namespace meeting {
namespace core {

enum class MeetingErrorCode {
    kOk = 0,
    kMeetingNotFound = 1,
    kMeetingEnded = 2,
    kParticipantExists = 3,
    kMeetingFull = 4,
    kPermissionDenied = 5,
    kInvalidState = 6,
};

inline void ErrorToProto(MeetingErrorCode code
                        , const meeting::common::Status& status
                        , proto::common::Error* error_proto) {
    if (error_proto == nullptr) {
        return;
    }
    error_proto->set_code(static_cast<int32_t>(code));
    error_proto->set_message(status.Message());
}

} // namespace core
} // namespace meeting