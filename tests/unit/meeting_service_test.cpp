#include "server/meeting_service_impl.hpp"
#include "meeting_service.grpc.pb.h"

#include <gtest/gtest.h>

using namespace meeting::server;

class MeetingServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        service_ = std::make_unique<MeetingServiceImpl>();
    }

    std::unique_ptr<MeetingServiceImpl> service_;
    grpc::ServerContext context_;
};

TEST_F(MeetingServiceTest, CreateMeetingSuccess) {
    proto::meeting::CreateMeetingRequest request;
    request.set_session_token("1001");
    request.set_topic("Daily Standup");
    proto::meeting::CreateMeetingResponse response;
    auto status = service_->CreateMeeting(&context_, &request, &response);

    EXPECT_TRUE(status.ok());
    EXPECT_EQ(response.error().code(), 0);
    EXPECT_EQ(response.meeting().topic(), "Daily Standup");
}

TEST_F(MeetingServiceTest, JoinLeaveAndEndFlow) {
    // Create Meeting
    proto::meeting::CreateMeetingRequest create_request;
    create_request.set_session_token("1001");
    create_request.set_topic("Daily Standup");
    proto::meeting::CreateMeetingResponse create_response;
    auto create_status = service_->CreateMeeting(&context_, &create_request, &create_response);
    ASSERT_TRUE(create_status.ok());

    const std::string meeting_id = create_response.meeting().meeting_id();

    // Join Meeting
    proto::meeting::JoinMeetingRequest join_request;
    join_request.set_meeting_id(meeting_id);
    join_request.set_session_token("2001");
    proto::meeting::JoinMeetingResponse join_response;
    auto join_status = service_->JoinMeeting(&context_, &join_request, &join_response);
    ASSERT_TRUE(join_status.ok());
    EXPECT_EQ(join_response.meeting().participant_ids_size(), 2);

    // Leave Meeting
    proto::meeting::LeaveMeetingRequest leave_request;
    leave_request.set_meeting_id(meeting_id);
    leave_request.set_session_token("2001");
    proto::meeting::LeaveMeetingResponse leave_response;
    auto leave_status = service_->LeaveMeeting(&context_, &leave_request, &leave_response);
    ASSERT_TRUE(leave_status.ok());

    // End Meeting
    proto::meeting::EndMeetingRequest end_request;
    end_request.set_meeting_id(meeting_id);
    end_request.set_session_token("1001");
    proto::meeting::EndMeetingResponse end_response;
    auto end_status = service_->EndMeeting(&context_, &end_request, &end_response);
    ASSERT_TRUE(end_status.ok());

    // Verify Meeting Ended
    proto::meeting::GetMeetingRequest get_request;
    get_request.set_meeting_id(meeting_id);
    proto::meeting::GetMeetingResponse get_response;
    auto get_status = service_->GetMeeting(&context_, &get_request, &get_response);
    ASSERT_TRUE(get_status.ok());
    EXPECT_EQ(get_response.meeting().state(), "ENDED");
}
