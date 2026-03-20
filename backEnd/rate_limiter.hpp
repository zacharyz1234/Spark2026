#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

class RateLimiter {
public:
    struct Config {
        int maxRequests;   // maximum requests allowed in the window
        int windowSeconds; // sliding window duration in seconds
    };

    explicit RateLimiter(std::unordered_map<std::string, Config> endpoint_limits)
        : limits_(std::move(endpoint_limits)) {}

    struct Result {
        bool allowed;
        int retryAfter; // seconds until oldest timestamp expires; 0 when allowed
    };

    // Thread-safe. Evicts stale timestamps, then checks and (if allowed) records.
    Result check(const std::string& ip, const std::string& path) {
        // Paths not in limits_ are always allowed, no timestamp recorded
        auto cfgIt = limits_.find(path);
        if (cfgIt == limits_.end()) {
            return {true, 0};
        }

        const Config& cfg = cfgIt->second;
        std::string key = ip + "|" + path;
        auto window = std::chrono::seconds(cfg.windowSeconds);
        auto now = std::chrono::steady_clock::now();

        std::lock_guard<std::mutex> lock(mutex_);

        Entry& entry = table_[key];

        // Evict timestamps older than windowSeconds
        while (!entry.timestamps.empty() &&
               (now - entry.timestamps.front()) >= window) {
            entry.timestamps.pop_front();
        }

        if (static_cast<int>(entry.timestamps.size()) >= cfg.maxRequests) {
            // Deny: compute retryAfter from oldest accepted timestamp
            auto expiry = entry.timestamps.front() + window;
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(expiry - now).count();
            int retryAfter = static_cast<int>(diff);
            if (retryAfter < 1) retryAfter = 1;
            return {false, retryAfter};
        }

        // Allow: record timestamp
        entry.timestamps.push_back(now);
        return {true, 0};
    }

private:
    struct Entry {
        std::deque<std::chrono::steady_clock::time_point> timestamps;
    };

    std::unordered_map<std::string, Config> limits_;
    std::unordered_map<std::string, Entry> table_; // key: "ip|path"
    std::mutex mutex_;
};
