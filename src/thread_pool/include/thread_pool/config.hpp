#pragma once
#include "thread_pool/fwd.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <optional>
#include <mutex>

namespace thread_pool {

class ThreadPoolConfigLoader {
public:
    ThreadPoolConfigLoader() = default;
    ~ThreadPoolConfigLoader() = default;

    ThreadPoolConfigLoader(const ThreadPoolConfigLoader&) = delete;
    ThreadPoolConfigLoader& operator=(const ThreadPoolConfigLoader&) = delete;

    // Return loader by move
    ThreadPoolConfigLoader(ThreadPoolConfigLoader&& other) noexcept;
    ThreadPoolConfigLoader& operator=(ThreadPoolConfigLoader&& other) noexcept;
    // Application layer
    static std::optional<ThreadPoolConfigLoader> FromFile(const std::string& filepath);
    static std::optional<ThreadPoolConfigLoader> FromString(const std::string& json);
    static std::optional<ThreadPoolConfigLoader> FromJson(const nlohmann::json& jcfg);

    bool Ready() const noexcept;
    ThreadPoolConfig GetConfig() const;
    std::string Dump() const;
private:
    // Input layer
    bool LoadFromFile(const std::string& filepath);
    bool LoadFromString(const std::string& json);
    bool LoadFromJson(const nlohmann::json& jcfg);
private:
    // Raw structure
    struct RawConfig{
        std::optional<std::size_t> queue_cap;               // task queue capacity
        std::optional<std::size_t> core_threads;            // core thread count
        std::optional<std::size_t> max_threads;             // maximum thread count
        std::optional<int>         load_check_interval_ms;  // load balancer sampling interval (ms)
        std::optional<int>         keep_alive_ms;           // idle thread keep-alive time (ms)
        std::optional<double>      scale_up_threshold;      // busy ratio upper bound; scale up when exceeded
        std::optional<double>      scale_down_threshold;    // busy ratio lower bound; scale down when below
        std::optional<std::size_t> pending_hi;              // pending threshold (upper)
        std::optional<std::size_t> pending_low;             // pending threshold (lower)
        std::optional<std::size_t> debounce_hits;           // debounce hit count
        std::optional<std::size_t> cooldown_ms;             // cooldown after capacity change (ms)
        std::optional<std::string> queue_policy;            // backpressure policy
    };
    // Parsing layer
    static RawConfig ParseRaw(const nlohmann::json& jcfg);
    static QueueFullPolicy ParsePolicy(const std::string& policy);

    // Validation/Normalization layer
    static ThreadPoolConfig Normalize(const RawConfig& raw);

    // Input helper
    bool LoadJson(const nlohmann::json& jcfg, const std::string& source_desc);

    // Output helper
    static nlohmann::json ToJson(const ThreadPoolConfig& cfg);
private:
    std::optional<ThreadPoolConfig> config_;
    mutable std::mutex cfg_mtx_;
};

}