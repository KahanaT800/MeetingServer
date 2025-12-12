#include <gtest/gtest.h>

#include "storage/mysql/connection_pool.hpp"
#include "storage/mysql/meeting_repository.hpp"

#include <cstdlib>
#include <sstream>
#include <string_view>

namespace {

std::string GetEnvOr(const char* name, const std::string& fallback) {
    if (const char* value = std::getenv(name)) {
        return value;
    }
    return fallback;
}

void ExecuteSQL(meeting::storage::ConnectionPool& pool, const std::string& sql) {
    auto lease_or = pool.Acquire();
    ASSERT_TRUE(lease_or.IsOk()) << lease_or.GetStatus().Message();
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    ASSERT_EQ(0, mysql_real_query(conn, sql.c_str(), sql.size())) << mysql_error(conn);
}

std::uint64_t InsertUser(meeting::storage::ConnectionPool& pool, const std::string& name) {
    auto lease_or = pool.Acquire();
    if (!lease_or.IsOk()) {
        ADD_FAILURE() << lease_or.GetStatus().Message();
        return 0;
    }
    auto lease = std::move(lease_or.Value());
    MYSQL* conn = lease.Raw();
    std::ostringstream sql;
    sql << "INSERT INTO users (user_uuid, username, display_name, email, password_hash, salt) VALUES ("
        << "'uuid-" << name << "', "
        << "'" << name << "', "
        << "'display-" << name << "', "
        << "'" << name << "@example.com', "
        << "'hash-" << name << "', "
        << "'salt-" << name << "')";
    if (mysql_real_query(conn, sql.str().c_str(), sql.str().size()) != 0) {
        ADD_FAILURE() << mysql_error(conn);
        return 0;
    }
    return static_cast<std::uint64_t>(mysql_insert_id(conn));
}

class MysqlMeetingRepositoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        meeting::storage::Options options;
        options.host = GetEnvOr("MEETING_DB_HOST", "127.0.0.1");
        options.port = static_cast<std::uint16_t>(std::stoi(GetEnvOr("MEETING_DB_PORT", "3306")));
        options.user = GetEnvOr("MEETING_DB_USER", "root");
        options.password = GetEnvOr("MEETING_DB_PASSWORD", "");
        options.database = GetEnvOr("MEETING_DB_NAME", "meeting");
        pool_ = std::make_shared<meeting::storage::ConnectionPool>(options);
        auto conn = pool_->Acquire();
        ASSERT_TRUE(conn.IsOk()) << conn.GetStatus().Message();
        repo_ = std::make_unique<meeting::storage::MySqlMeetingRepository>(pool_);
        ExecuteSQL(*pool_, "DELETE FROM meeting_participants");
        ExecuteSQL(*pool_, "DELETE FROM meetings");
        ExecuteSQL(*pool_, "DELETE FROM users");
    }

    void TearDown() override {
        if (pool_) {
            ExecuteSQL(*pool_, "DELETE FROM meeting_participants");
            ExecuteSQL(*pool_, "DELETE FROM meetings");
            ExecuteSQL(*pool_, "DELETE FROM users");
        }
    }

    std::shared_ptr<meeting::storage::ConnectionPool> pool_;
    std::unique_ptr<meeting::storage::MySqlMeetingRepository> repo_;
};

meeting::core::MeetingData MakeMeeting(std::uint64_t organizer_id, const std::string& org_suffix) {
    meeting::core::MeetingData data;
    data.meeting_id = "meeting_" + org_suffix;
    data.meeting_code = "code" + org_suffix;
    data.organizer_id = organizer_id;
    data.topic = "topic" + org_suffix;
    data.state = meeting::core::MeetingState::kScheduled;
    data.created_at = 0;
    data.updated_at = 0;
    data.participants.push_back(organizer_id);
    return data;
}

TEST_F(MysqlMeetingRepositoryTest, CreateAndFetch) {
    if (!repo_) {
        GTEST_SKIP();
    }
    auto organizer = InsertUser(*pool_, "org1");
    ASSERT_NE(organizer, 0u);
    auto data = MakeMeeting(organizer, "org1");
    auto created = repo_->CreateMeeting(data);
    ASSERT_TRUE(created.IsOk()) << created.GetStatus().Message();

    auto fetched = repo_->GetMeeting(data.meeting_id);
    ASSERT_TRUE(fetched.IsOk()) << fetched.GetStatus().Message();
    EXPECT_EQ(fetched.Value().organizer_id, data.organizer_id);
}

TEST_F(MysqlMeetingRepositoryTest, AddAndRemoveParticipant) {
    if (!repo_) {
        GTEST_SKIP();
    }
    auto organizer = InsertUser(*pool_, "org2");
    auto participant = InsertUser(*pool_, "userA");
    ASSERT_NE(organizer, 0u);
    ASSERT_NE(participant, 0u);
    auto data = MakeMeeting(organizer, "org2");
    auto created = repo_->CreateMeeting(data);
    ASSERT_TRUE(created.IsOk());

    auto add_status = repo_->AddParticipant(data.meeting_id, participant, false);
    ASSERT_TRUE(add_status.IsOk());
    auto list = repo_->ListParticipants(data.meeting_id);
    ASSERT_TRUE(list.IsOk());
    EXPECT_GE(list.Value().size(), 1u);

    auto rm = repo_->RemoveParticipant(data.meeting_id, participant);
    ASSERT_TRUE(rm.IsOk());
}

}
