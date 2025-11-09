#include "core/meeting/meeting_manager.hpp"

#include <gtest/gtest.h>

using namespace meeting::core;
using namespace meeting::common;

class MeetingManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = std::make_unique<MeetingManager>();
    }

    std::unique_ptr<MeetingManager> manager_;
};

TEST_F(MeetingManagerTest, CreateAndGetMeeting) {
    CreateMeetingCommand command{"organizer-1", "Daily Standup"};
    auto created = manager_->CreateMeeting(command);
    ASSERT_TRUE(created.IsOk());

    auto fetched = manager_->GetMeeting(created.Value().meeting_id);
    ASSERT_TRUE(fetched.IsOk());

    EXPECT_EQ(fetched.Value().topic, "Daily Standup");
    EXPECT_EQ(fetched.Value().organizer_id, "organizer-1");
    EXPECT_EQ(fetched.Value().state, MeetingState::kScheduled);
}

TEST_F(MeetingManagerTest, JoinMeetingSuccess) {
    CreateMeetingCommand create_cmd{"organizer-1", "Daily Standup"};
    auto created = manager_->CreateMeeting(create_cmd);
    ASSERT_TRUE(created.IsOk());

    JoinMeetingCommand join_cmd{created.Value().meeting_id, "participant-1"};
    auto join_result = manager_->JoinMeeting(join_cmd);
    ASSERT_TRUE(join_result.IsOk());

    auto fetched = manager_->GetMeeting(created.Value().meeting_id);
    ASSERT_TRUE(fetched.IsOk());
    EXPECT_EQ(fetched.Value().participants.size(), 2u);
    EXPECT_EQ(fetched.Value().participants[0], "organizer-1");
    EXPECT_EQ(fetched.Value().participants[1], "participant-1");
}

TEST_F(MeetingManagerTest, LeaveMeetingSuccess) {
    CreateMeetingCommand create_cmd{"organizer-1", "Daily Standup"};
    auto created = manager_->CreateMeeting(create_cmd);
    ASSERT_TRUE(created.IsOk());

    JoinMeetingCommand join_cmd{created.Value().meeting_id, "participant-1"};
    auto join_result = manager_->JoinMeeting(join_cmd);
    ASSERT_TRUE(join_result.IsOk());

    LeaveMeetingCommand leave_cmd{created.Value().meeting_id, "participant-1"};
    auto leave_result = manager_->LeaveMeeting(leave_cmd);
    ASSERT_TRUE(leave_result.IsOk());

    auto fetched = manager_->GetMeeting(created.Value().meeting_id);
    ASSERT_TRUE(fetched.IsOk());
    EXPECT_EQ(fetched.Value().participants.size(), 1u);
    EXPECT_EQ(fetched.Value().participants[0], "organizer-1");
}

TEST_F(MeetingManagerTest, EndMeetingByOrganizer) {
    CreateMeetingCommand create_cmd{"organizer-1", "Daily Standup"};
    auto created = manager_->CreateMeeting(create_cmd);
    ASSERT_TRUE(created.IsOk());

    EndMeetingCommand end_cmd{created.Value().meeting_id, "organizer-1"};
    auto end_result = manager_->EndMeeting(end_cmd);
    ASSERT_TRUE(end_result.IsOk());

    auto fetched = manager_->GetMeeting(created.Value().meeting_id);
    ASSERT_TRUE(fetched.IsOk());
    EXPECT_EQ(fetched.Value().state, MeetingState::kEnded);
}

TEST_F(MeetingManagerTest, EndMeetingByNonOrganizerFails) {
    CreateMeetingCommand create_cmd{"organizer-1", "Daily Standup"};
    auto created = manager_->CreateMeeting(create_cmd);
    ASSERT_TRUE(created.IsOk());

    EndMeetingCommand end_cmd{created.Value().meeting_id, "participant-1"};
    auto end_result = manager_->EndMeeting(end_cmd);
    EXPECT_FALSE(end_result.IsOk());
    EXPECT_EQ(end_result.Code(), StatusCode::kUnauthenticated);

    auto fetched = manager_->GetMeeting(created.Value().meeting_id);
    ASSERT_TRUE(fetched.IsOk());
    EXPECT_EQ(fetched.Value().state, MeetingState::kScheduled);
}

TEST_F(MeetingManagerTest, JoinFailsWhenMeetingEnded) {
    CreateMeetingCommand create_cmd{"organizer-1", "Daily Standup"};
    auto created = manager_->CreateMeeting(create_cmd);
    ASSERT_TRUE(created.IsOk());

    EndMeetingCommand end_cmd{created.Value().meeting_id, "organizer-1"};
    auto end_result = manager_->EndMeeting(end_cmd);
    ASSERT_TRUE(end_result.IsOk());

    JoinMeetingCommand join_cmd{created.Value().meeting_id, "participant-1"};
    auto join_result = manager_->JoinMeeting(join_cmd);
    EXPECT_FALSE(join_result.IsOk());
    EXPECT_EQ(join_result.GetStatus().Code(), StatusCode::kInvalidArgument);
}