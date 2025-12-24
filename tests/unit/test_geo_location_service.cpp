#include "geo/geo_location_service.hpp"

#include <gtest/gtest.h>

using meeting::geo::GeoLocationService;

TEST(GeoLocationServiceTest, InvalidIp) {
    GeoLocationService svc("nonexistent.mmdb");
    auto res = svc.Lookup("not-an-ip");
    EXPECT_FALSE(res.IsOk());
    EXPECT_EQ(res.GetStatus().Code(), meeting::common::StatusCode::kInvalidArgument);
}

TEST(GeoLocationServiceTest, PrivateIp) {
    GeoLocationService svc("nonexistent.mmdb");
    auto res = svc.Lookup("192.168.1.1");
    EXPECT_TRUE(res.IsOk());
    EXPECT_TRUE(res.Value().is_private);
}

TEST(GeoLocationServiceTest, MissingDb) {
    GeoLocationService svc("nonexistent.mmdb");
    auto res = svc.Lookup("8.8.8.8");
    EXPECT_FALSE(res.IsOk());
    EXPECT_EQ(res.GetStatus().Code(), meeting::common::StatusCode::kUnavailable);
}

TEST(GeoLocationServiceTest, LookupWithRealDbIfExists) {
#ifdef GEOIP_TEST_DB_PATH
    const char* default_path = GEOIP_TEST_DB_PATH;
#else
    const char* default_path = nullptr;
#endif
    std::string db_path;
    if (const char* env = std::getenv("GEOIP_DB_PATH")) {
        db_path = env;
    } else if (default_path) {
        db_path = default_path;
    }
    if (db_path.empty()) {
        GTEST_SKIP() << "No GeoIP DB path provided";
    }
    if (FILE* f = fopen(db_path.c_str(), "rb")) {
        fclose(f);
    } else {
        GTEST_SKIP() << "GeoIP DB not found at " << db_path;
    }

    GeoLocationService svc(db_path);
    ASSERT_TRUE(svc.IsAvailable()) << "DB not available at " << db_path;

    auto res = svc.Lookup("8.8.8.8");
    ASSERT_TRUE(res.IsOk()) << res.GetStatus().Message();
    const auto& info = res.Value();
    EXPECT_EQ(info.iso_code, "US");
    EXPECT_FALSE(info.country.empty());
}
