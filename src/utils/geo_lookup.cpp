#include "geo/geo_location_service.hpp"
#include "common/status.hpp"
#include "common/status_or.hpp"

#include <cstdlib>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <IP_ADDRESS> [DB_PATH]\n";
        std::cerr << "Example: " << argv[0] << " 8.8.8.8 ip_data/GeoLite2-City.mmdb\n";
        return 1;
    }

    std::string ip = argv[1];
    std::string db_path = argc >= 3 ? argv[2] : "ip_data/GeoLite2-City.mmdb";

    if (const char* env_path = std::getenv("GEOIP_DB_PATH")) {
        db_path = env_path;
    }

    meeting::geo::GeoLocationService service(db_path);

    std::cout << "GeoIP Database: " << db_path << "\n";
    std::cout << "Database available: " << (service.IsAvailable() ? "Yes" : "No") << "\n";
    std::cout << "Querying IP: " << ip << "\n\n";

    auto result = service.Lookup(ip);
    if (!result.IsOk()) {
        auto st = result.GetStatus();
        std::cerr << "Error: " << meeting::common::StatusCodeToString(st.Code())
                  << " - " << st.Message() << "\n";
        return 1;
    }

    const auto& info = result.Value();
    std::cout << "{\n";
    std::cout << "  \"ip\": \"" << ip << "\",\n";
    std::cout << "  \"country\": \"" << info.country << "\",\n";
    std::cout << "  \"iso_code\": \"" << info.iso_code << "\",\n";
    std::cout << "  \"region\": \"" << info.region << "\",\n";
    std::cout << "  \"city\": \"" << info.city << "\",\n";
    std::cout << "  \"timezone\": \"" << info.timezone << "\",\n";
    std::cout << "  \"latitude\": " << info.latitude << ",\n";
    std::cout << "  \"longitude\": " << info.longitude << ",\n";
    std::cout << "  \"is_private\": " << (info.is_private ? "true" : "false") << "\n";
    std::cout << "}\n";
    return 0;
}