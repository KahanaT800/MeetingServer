#include "server/meeting_service_impl.hpp"
#include "server/user_service_impl.hpp"
#include "meeting_service.grpc.pb.h"
#include "test_mysql_utils.hpp"

#include <gtest/gtest.h>

using namespace meeting::server;

class MeetingServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        testutils::ClearMysqlTestData();
        user_service_ = std::make_unique<UserServiceImpl>();
        service_ = std::make_unique<MeetingServiceImpl>();
        organizer_token_ = RegisterAndLogin("organizer_user", "org@example.com");
        participant_token_ = RegisterAndLogin("participant_user", "participant@example.com");
    }

    std::string RegisterAndLogin(const std::string& user_name, const std::string& email) {
        proto::user::RegisterRequest reg;
        reg.set_user_name(user_name);
        reg.set_password("password123");
        reg.set_email(email);
        proto::user::RegisterResponse reg_resp;
        grpc::ServerContext ctx1;
        auto reg_status = user_service_->Register(&ctx1, &reg, &reg_resp);
        EXPECT_TRUE(reg_status.ok()) << reg_status.error_message();
        EXPECT_EQ(reg_resp.error().code(), 0) << reg_resp.error().message();

        proto::user::LoginRequest login;
        login.set_user_name(user_name);
        login.set_password("password123");
        proto::user::LoginResponse login_resp;
        grpc::ServerContext ctx2;
        auto login_status = user_service_->Login(&ctx2, &login, &login_resp);
        EXPECT_TRUE(login_status.ok()) << login_status.error_message();
        EXPECT_EQ(login_resp.error().code(), 0) << login_resp.error().message();
        return login_resp.session_token();
    }

    std::unique_ptr<UserServiceImpl> user_service_;
    std::unique_ptr<MeetingServiceImpl> service_;
    grpc::ServerContext context_;
    std::string organizer_token_;
    std::string participant_token_;
};

TEST_F(MeetingServiceTest, CreateMeetingSuccess) {
    proto::meeting::CreateMeetingRequest request;
    request.set_session_token(organizer_token_);
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
    create_request.set_session_token(organizer_token_);
    create_request.set_topic("Daily Standup");
    proto::meeting::CreateMeetingResponse create_response;
    auto create_status = service_->CreateMeeting(&context_, &create_request, &create_response);
    ASSERT_TRUE(create_status.ok());

    const std::string meeting_id = create_response.meeting().meeting_id();

    // Join Meeting
    proto::meeting::JoinMeetingRequest join_request;
    join_request.set_meeting_id(meeting_id);
    join_request.set_session_token(participant_token_);
    proto::meeting::JoinMeetingResponse join_response;
    auto join_status = service_->JoinMeeting(&context_, &join_request, &join_response);
    ASSERT_TRUE(join_status.ok());
    EXPECT_EQ(join_response.meeting().participant_ids_size(), 2);

    // Leave Meeting
    proto::meeting::LeaveMeetingRequest leave_request;
    leave_request.set_meeting_id(meeting_id);
    leave_request.set_session_token(participant_token_);
    proto::meeting::LeaveMeetingResponse leave_response;
    auto leave_status = service_->LeaveMeeting(&context_, &leave_request, &leave_response);
    ASSERT_TRUE(leave_status.ok());

    // End Meeting
    proto::meeting::EndMeetingRequest end_request;
    end_request.set_meeting_id(meeting_id);
    end_request.set_session_token(organizer_token_);
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
