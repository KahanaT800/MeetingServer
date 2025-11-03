#include "thread_pool/config.hpp"
#include "logger.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace thread_pool {
    ThreadPoolConfigLoader::ThreadPoolConfigLoader(ThreadPoolConfigLoader&& other) noexcept {
        std::lock_guard<std::mutex> lk(other.cfg_mtx_);
        config_ = std::move(other.config_); // Move only the config
    }
    ThreadPoolConfigLoader& ThreadPoolConfigLoader::operator=(ThreadPoolConfigLoader&& other) noexcept {
        if (this != &other) {
            std::scoped_lock lk(cfg_mtx_, other.cfg_mtx_);
            config_ = std::move(other.config_); // Move only the config
        }
        return *this;
    }
    // Application layer
    std::optional<ThreadPoolConfigLoader> ThreadPoolConfigLoader::FromFile(const std::string& filepath) {
        ThreadPoolConfigLoader loader;
        if (!loader.LoadFromFile(filepath)) {
            return std::nullopt;
        }
        return loader;
    }

    std::optional<ThreadPoolConfigLoader> ThreadPoolConfigLoader::FromString(const std::string& json) {
        ThreadPoolConfigLoader loader;
        if (!loader.LoadFromString(json)) {
            return std::nullopt;
        }
        return loader;
    }

    std::optional<ThreadPoolConfigLoader> ThreadPoolConfigLoader::FromJson(const nlohmann::json& jcfg) {
        ThreadPoolConfigLoader loader;
        if (!loader.LoadFromJson(jcfg)) {
            return std::nullopt;
        }
        return loader;
    }

    // Input layer
    bool ThreadPoolConfigLoader::LoadFromFile(const std::string& filepath) {
        std::ifstream ifs(filepath);
        if (!ifs.is_open()) {
            TP_LOG_ERROR("ThreadPoolConfigLoader: cannot open config file {}", filepath);
            return false;
        }
        try {
            nlohmann::json jcfg;
            ifs >> jcfg;
            return LoadJson(jcfg, filepath);
        } catch (const std::exception& e) {
            TP_LOG_ERROR("ThreadPoolConfigLoader: failed to parse {}: {}", filepath, e.what());
            return false;
        }
    }

    bool ThreadPoolConfigLoader::LoadFromString(const std::string& json) {
        try {
            nlohmann::json jcfg = nlohmann::json::parse(json);
            return LoadJson(jcfg, "<string>");
        } catch (const std::exception& e) {
            TP_LOG_ERROR("ThreadPoolConfigLoader: failed to parse string config: {}", e.what());
            return false;
        }
    }

    bool ThreadPoolConfigLoader::LoadFromJson(const nlohmann::json& jcfg) {
        try {
            return LoadJson(jcfg, "<json>");
        } catch (const std::exception& e) {
            TP_LOG_ERROR("ThreadPoolConfigLoader: failed to load json config: {}", e.what());
            return false;
        }
    }

    bool ThreadPoolConfigLoader::LoadJson(const nlohmann::json& jcfg, const std::string& source_desc) {
        try {
            RawConfig raw = ParseRaw(jcfg);
            ThreadPoolConfig cfg = Normalize(raw);
            TP_LOG_INFO(
                "ThreadPool config loaded from {} (queue_cap={} core_threads={} max_threads={} pending_hi={} pending_low={} policy={})",
                source_desc,
                cfg.queue_cap,
                cfg.core_threads,
                cfg.max_threads,
                cfg.pending_hi,
                cfg.pending_low,
                cfg.queue_policy);
            {
                std::lock_guard<std::mutex> lk(cfg_mtx_);
                config_ = std::move(cfg);
            }
            return true;
        } catch (const std::exception& e) {
            TP_LOG_ERROR("ThreadPoolConfigLoader: failed to normalize config from {}: {}", source_desc, e.what());
            return false;
        }
    }
    // Parsing layer
    ThreadPoolConfigLoader::RawConfig ThreadPoolConfigLoader::ParseRaw(const nlohmann::json& jcfg) {
        RawConfig raw;

        // Parse each field
        if (jcfg.contains("queue_cap")) {
            raw.queue_cap = jcfg.at("queue_cap").get<std::size_t>();
        }
        if (jcfg.contains("core_threads")) {
            raw.core_threads = jcfg.at("core_threads").get<std::size_t>();
        }
        if (jcfg.contains("max_threads")) {
            raw.max_threads = jcfg.at("max_threads").get<std::size_t>();
        }
        if (jcfg.contains("load_check_interval_ms")) {
            raw.load_check_interval_ms = jcfg.at("load_check_interval_ms").get<int>();
        }
        if (jcfg.contains("keep_alive_ms")) {
            raw.keep_alive_ms = jcfg.at("keep_alive_ms").get<int>();
        }
        if (jcfg.contains("scale_up_threshold")) {
            raw.scale_up_threshold = jcfg.at("scale_up_threshold").get<double>();
        }
        if (jcfg.contains("scale_down_threshold")) {
            raw.scale_down_threshold = jcfg.at("scale_down_threshold").get<double>();
        }
        if (jcfg.contains("pending_hi")) {
            raw.pending_hi = jcfg.at("pending_hi").get<std::size_t>();
        }
        if (jcfg.contains("pending_low")) {
            raw.pending_low = jcfg.at("pending_low").get<std::size_t>();
        }
        if (jcfg.contains("debounce_hits")) {
            raw.debounce_hits = jcfg.at("debounce_hits").get<std::size_t>();
        }
        if (jcfg.contains("cooldown_ms")) {
            raw.cooldown_ms = jcfg.at("cooldown_ms").get<std::size_t>();
        }
        if (jcfg.contains("queue_policy")) {
            raw.queue_policy = jcfg.at("queue_policy").get<std::string>();
        }

        return raw;
    }

    QueueFullPolicy ThreadPoolConfigLoader::ParsePolicy(const std::string& policy) {
        if (policy == "Block") {
            return QueueFullPolicy::Block;
        } else if (policy == "Discard") {
            return QueueFullPolicy::Discard;
        } else if (policy == "Overwrite") {
            return QueueFullPolicy::Overwrite;
        } else {
            throw std::invalid_argument("Invalid queue_policy: " + policy);
        }
    }

    // Validation/Normalization layer
    ThreadPoolConfig ThreadPoolConfigLoader::Normalize(const RawConfig& raw) {
        ThreadPoolConfig cfg;

        // Assign fields
        if (raw.queue_cap.has_value()) {
            cfg.queue_cap = raw.queue_cap.value();
        }
        if (raw.core_threads.has_value()) {
            cfg.core_threads = raw.core_threads.value();
        }
        if (raw.max_threads.has_value()) {
            cfg.max_threads = raw.max_threads.value();
        }
        if (raw.load_check_interval_ms.has_value()) {
            cfg.load_check_interval = std::chrono::milliseconds{raw.load_check_interval_ms.value()};
        }
        if (raw.keep_alive_ms.has_value()) {
            cfg.keep_alive = std::chrono::milliseconds{raw.keep_alive_ms.value()};
        }
        if (raw.scale_up_threshold.has_value()) {
            cfg.scale_up_threshold = raw.scale_up_threshold.value();
        }
        if (raw.scale_down_threshold.has_value()) {
            cfg.scale_down_threshold = raw.scale_down_threshold.value();
        }
        if (raw.pending_hi.has_value()) {
            cfg.pending_hi = raw.pending_hi.value();
        }
        if (raw.pending_low.has_value()) {
            cfg.pending_low = raw.pending_low.value();
        }
        if (raw.debounce_hits.has_value()) {
            cfg.debounce_hits = raw.debounce_hits.value();
        }
        if (raw.cooldown_ms.has_value()) {
            cfg.cooldown = std::chrono::milliseconds{raw.cooldown_ms.value()};
        }
        if (raw.queue_policy.has_value()) {
            cfg.queue_policy = ParsePolicy(raw.queue_policy.value());
        }

        // Sanity adjustments
        cfg.core_threads = std::max<std::size_t>(1, cfg.core_threads);
        cfg.max_threads = std::max(cfg.core_threads, cfg.max_threads);
        cfg.pending_low = std::min(cfg.pending_hi, cfg.pending_low);
        cfg.debounce_hits = std::max<std::size_t>(1, cfg.debounce_hits);
        return cfg;
    }

    // Application layer
    bool ThreadPoolConfigLoader::Ready() const noexcept {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        return config_.has_value();
    }

    ThreadPoolConfig ThreadPoolConfigLoader::GetConfig() const {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        if (!config_.has_value()) {
            throw std::runtime_error("ThreadPoolConfigLoader: config not ready");
        }
        return config_.value_or(ThreadPoolConfig{});
    }

    std::string ThreadPoolConfigLoader::Dump() const {
        std::lock_guard<std::mutex> lk(cfg_mtx_);
        if (!config_.has_value()) {
            return "{}";
        }
        return ToJson(config_.value()).dump(2);
    }

    // Output utilities
    nlohmann::json ThreadPoolConfigLoader::ToJson(const ThreadPoolConfig& cfg) {
        nlohmann::json jcfg;
        jcfg["queue_cap"] = cfg.queue_cap;
        jcfg["core_threads"] = cfg.core_threads;
        jcfg["max_threads"] = cfg.max_threads;
        jcfg["load_check_interval_ms"] = cfg.load_check_interval.count();
        jcfg["keep_alive_ms"] = cfg.keep_alive.count();
        jcfg["scale_up_threshold"] = cfg.scale_up_threshold;
        jcfg["scale_down_threshold"] = cfg.scale_down_threshold;
        jcfg["pending_hi"] = cfg.pending_hi;
        jcfg["pending_low"] = cfg.pending_low;
        jcfg["debounce_hits"] = cfg.debounce_hits;
        jcfg["cooldown_ms"] = cfg.cooldown.count();
        switch (cfg.queue_policy) {
            case QueueFullPolicy::Block:
                jcfg["queue_policy"] = "Block";
                break;
            case QueueFullPolicy::Discard:
                jcfg["queue_policy"] = "Discard";
                break;
            case QueueFullPolicy::Overwrite:
                jcfg["queue_policy"] = "Overwrite";
                break;
        }
        return jcfg;
    }

}
