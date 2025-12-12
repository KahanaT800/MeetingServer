#include <gtest/gtest.h>

#include "server/user_service_impl.hpp"
#include "server/meeting_service_impl.hpp"
#include "common/config_loader.hpp"
#include "storage/mysql/connection_pool.hpp"
#include "user_service.grpc.pb.h"
#include "meeting_service.grpc.pb.h"

#include <grpcpp/grpcpp.h>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string_view>

namespace {

std::string GetEnvOr(const char* name, const std::string& fallback) {
    if (const char* v = std::getenv(name)) {
        return v;
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

class MySqlFlowTest : public ::testing::Test {
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
        
        // 清理测试数据
        ExecuteSQL(*pool_, "DELETE FROM meeting_participants");
        ExecuteSQL(*pool_, "DELETE FROM meetings");
        ExecuteSQL(*pool_, "DELETE FROM user_sessions");
        ExecuteSQL(*pool_, "DELETE FROM users");

        // 写一个临时配置文件，开启 MySQL backend，供 GlobalConfig 使用
        temp_config_path_ = "/tmp/meeting_app_test_config.json";
        std::ofstream ofs(temp_config_path_);
        ofs << "{\n"
            << "  \"server\": { \"host\": \"0.0.0.0\", \"port\": 50051 },\n"
            << "  \"logging\": { \"level\": \"error\", \"console\": true },\n"
            << "  \"thread_pool\": { \"config_path\": \"config/thread_pool.json\" },\n"
            << "  \"storage\": {\n"
            << "    \"mysql\": {\n"
            << "      \"host\": \"" << options.host << "\",\n"
            << "      \"port\": " << options.port << ",\n"
            << "      \"user\": \"" << options.user << "\",\n"
            << "      \"password\": \"" << options.password << "\",\n"
            << "      \"database\": \"" << options.database << "\",\n"
            << "      \"pool_size\": " << (options.pool_size == 0 ? 5 : options.pool_size) << ",\n"
            << "      \"connection_timeout_ms\": 500,\n"
            << "      \"read_timeout_ms\": 2000,\n"
            << "      \"write_timeout_ms\": 2000,\n"
            << "      \"enabled\": true\n"
            << "    }\n"
            << "  }\n"
            << "}\n";
        ofs.close();
        setenv("MEETING_SERVER_CONFIG", temp_config_path_.c_str(), 1);
    }

    void TearDown() override {
        if (pool_) {
            ExecuteSQL(*pool_, "DELETE FROM meeting_participants");
            ExecuteSQL(*pool_, "DELETE FROM meetings");
            ExecuteSQL(*pool_, "DELETE FROM user_sessions");
            ExecuteSQL(*pool_, "DELETE FROM users");
        }
        if (!temp_config_path_.empty()) {
            std::remove(temp_config_path_.c_str());
        }
    }

    std::shared_ptr<meeting::storage::ConnectionPool> pool_;
    std::unique_ptr<grpc::Server> server_;
    int selected_port_ = 0;
    std::string temp_config_path_;
};

} // namespace

TEST_F(MySqlFlowTest, EndToEndUserMeetingFlow) {
    meeting::common::AppConfig cfg = meeting::common::ConfigLoader::Load(meeting::common::GetConfigPath("app.example.json"));
    cfg.storage.mysql.enabled = true;
    meeting::common::InitLogger(cfg.logging);

    meeting::server::UserServiceImpl user_service(meeting::common::GetThreadPoolConfigPath());
    meeting::server::MeetingServiceImpl meeting_service(meeting::common::GetThreadPoolConfigPath());

    grpc::ServerBuilder builder;
    builder.RegisterService(&user_service);
    builder.RegisterService(&meeting_service);
    builder.AddListeningPort("0.0.0.0:0", grpc::InsecureServerCredentials(), &selected_port_);
    server_ = builder.BuildAndStart();
    ASSERT_TRUE(server_);

    auto channel = grpc::CreateChannel("localhost:" + std::to_string(selected_port_), grpc::InsecureChannelCredentials());
    auto user_stub = proto::user::UserService::NewStub(channel);
    auto meeting_stub = proto::meeting::MeetingService::NewStub(channel);

    // Register user A
    proto::user::RegisterRequest regA;
    regA.set_user_name("userA");
    regA.set_password("password123");
    regA.set_email("a@example.com");
    grpc::ClientContext ctxA;
    proto::user::RegisterResponse regRespA;
    auto s = user_stub->Register(&ctxA, regA, &regRespA);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(regRespA.error().code(), 0);

    // Register user B
    proto::user::RegisterRequest regB;
    regB.set_user_name("userB");
    regB.set_password("password123");
    regB.set_email("b@example.com");
    grpc::ClientContext ctxB;
    proto::user::RegisterResponse regRespB;
    s = user_stub->Register(&ctxB, regB, &regRespB);
    ASSERT_TRUE(s.ok());

    // Login user A
    proto::user::LoginRequest loginReq;
    loginReq.set_user_name("userA");
    loginReq.set_password("password123");
    grpc::ClientContext ctxLogin;
    proto::user::LoginResponse loginResp;
    s = user_stub->Login(&ctxLogin, loginReq, &loginResp);
    ASSERT_TRUE(s.ok());
    ASSERT_FALSE(loginResp.session_token().empty());

    // Create meeting
    proto::meeting::CreateMeetingRequest createReq;
    createReq.set_session_token(loginResp.session_token());
    createReq.set_topic("demo");
    grpc::ClientContext ctxCreate;
    proto::meeting::CreateMeetingResponse createResp;
    s = meeting_stub->CreateMeeting(&ctxCreate, createReq, &createResp);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(createResp.error().code(), 0);
    auto meeting_id = createResp.meeting().meeting_id();
    ASSERT_FALSE(meeting_id.empty());

    // User B login
    proto::user::LoginRequest loginReqB;
    loginReqB.set_user_name("userB");
    loginReqB.set_password("password123");
    grpc::ClientContext ctxLoginB;
    proto::user::LoginResponse loginRespB;
    s = user_stub->Login(&ctxLoginB, loginReqB, &loginRespB);
    ASSERT_TRUE(s.ok());

    // Join meeting as B
    proto::meeting::JoinMeetingRequest joinReq;
    joinReq.set_meeting_id(meeting_id);
    joinReq.set_session_token(loginRespB.session_token());
    grpc::ClientContext ctxJoin;
    proto::meeting::JoinMeetingResponse joinResp;
    s = meeting_stub->JoinMeeting(&ctxJoin, joinReq, &joinResp);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(joinResp.error().code(), 0);

    // Get meeting
    proto::meeting::GetMeetingRequest getReq;
    getReq.set_meeting_id(meeting_id);
    grpc::ClientContext ctxGet;
    proto::meeting::GetMeetingResponse getResp;
    s = meeting_stub->GetMeeting(&ctxGet, getReq, &getResp);
    ASSERT_TRUE(s.ok());
    ASSERT_EQ(getResp.error().code(), 0);
    EXPECT_EQ(getResp.meeting().meeting_id(), meeting_id);

    server_->Shutdown();
}
