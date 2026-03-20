#pragma once

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using sizeT = std::size_t;

class CorsValidator {
public:
    explicit CorsValidator() {
        const char* env = std::getenv("ALLOWED_ORIGINS");

        if (env == nullptr || env[0] == '\0') {
            wildcard_ = true;
            std::cerr << "[CORS] Warning: ALLOWED_ORIGINS not set — running in wildcard mode (Access-Control-Allow-Origin: *)\n";
            return;
        }

        // Parse comma-separated list
        std::string raw(env);
        sizeT start = 0;
        int index = 0;

        while (start <= raw.size()) {
            sizeT comma = raw.find(',', start); //finds the comma
            std::string token = (comma == std::string::npos)
                ? raw.substr(start)
                : raw.substr(start, comma - start);

            // Trim leading and falling whitespace
            sizeT lpos = token.find_first_not_of(" \t\r\n");
            sizeT rpos = token.find_last_not_of(" \t\r\n");

            if (lpos == std::string::npos) {
                // Empty after trimming
                std::cerr << "[CORS] Warning: skipping malformed (empty) token at index " << index << "\n";
            } else {
                allowlist_.push_back(token.substr(lpos, rpos - lpos + 1));
            }

            ++index;

            if (comma == std::string::npos) break;
            start = comma + 1;
        }

        if (allowlist_.empty()) {
            wildcard_ = true;
            std::cerr << "[CORS] Warning: no valid origins parsed — running in wildcard mode (Access-Control-Allow-Origin: *)\n";
        } else {
            wildcard_ = false;
            std::cout << "[CORS] Allowed origins:";
            for (const auto& o : allowlist_) {
                std::cout << " " << o;
            }
            std::cout << "\n";
        }
    }

    struct Result {
        bool allowed;
        std::string originHeader; // value for Access-Control-Allow-Origin, empty if blocked
    };

    Result validate(const std::string& origin) const {
        // No Origin header — always allow, no CORS header needed
        if (origin.empty()) {
            return {true, ""};
        }

        if (wildcard_) {
            return {true, "*"};
        }

        for (const auto& entry : allowlist_) {
            if (entry == origin) {
                return {true, entry};
            }
        }

        return {false, ""};
    }

    bool isWildcard() const {
        return wildcard_;
    }

private:
    std::vector<std::string> allowlist_;
    bool wildcard_ = false;
};
