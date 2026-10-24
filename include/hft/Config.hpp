#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hft::config {

inline std::string trim(std::string_view value) {
    std::size_t begin = 0;
    std::size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

inline std::string environment_name(std::string_view key) {
    std::string name = "HFT_";
    for (const char character : key) {
        name.push_back(character == '.' || character == '-'
                           ? '_'
                           : static_cast<char>(std::toupper(
                                 static_cast<unsigned char>(character))));
    }
    return name;
}

class Config {
public:
    void load(const std::string& path) {
        std::ifstream input(path);
        if (!input) {
            throw std::runtime_error("unable to open config: " + path);
        }
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(input, line)) {
            ++line_number;
            const std::string cleaned = trim(line);
            if (cleaned.empty() || cleaned.front() == '#') {
                continue;
            }
            const std::size_t separator = cleaned.find('=');
            if (separator == std::string::npos) {
                throw std::runtime_error("malformed config line " +
                                         std::to_string(line_number) + ": " + line);
            }
            const std::string key = trim(std::string_view(cleaned).substr(0, separator));
            const std::string value =
                trim(std::string_view(cleaned).substr(separator + 1));
            if (key.empty()) {
                throw std::runtime_error("empty config key on line " +
                                         std::to_string(line_number));
            }
            values_[key] = value;
        }
    }

    void set(std::string key, std::string value) {
        values_[std::move(key)] = std::move(value);
    }

    [[nodiscard]] bool contains(const std::string& key) const {
        return lookup(key).has_value();
    }

    [[nodiscard]] std::string get_string(const std::string& key,
                                         const std::string& fallback = {}) const {
        const auto value = lookup(key);
        return value ? *value : fallback;
    }

    [[nodiscard]] std::int64_t get_int(const std::string& key,
                                       std::int64_t fallback = 0) const {
        const auto value = lookup(key);
        if (!value) {
            return fallback;
        }
        try {
            std::size_t consumed = 0;
            const std::int64_t parsed = std::stoll(*value, &consumed);
            if (consumed != value->size()) {
                throw std::invalid_argument("trailing characters");
            }
            return parsed;
        } catch (const std::exception&) {
            throw std::runtime_error("invalid integer for config key " + key);
        }
    }

    [[nodiscard]] double get_double(const std::string& key,
                                    double fallback = 0.0) const {
        const auto value = lookup(key);
        if (!value) {
            return fallback;
        }
        try {
            std::size_t consumed = 0;
            const double parsed = std::stod(*value, &consumed);
            if (consumed != value->size()) {
                throw std::invalid_argument("trailing characters");
            }
            return parsed;
        } catch (const std::exception&) {
            throw std::runtime_error("invalid number for config key " + key);
        }
    }

    [[nodiscard]] bool get_bool(const std::string& key, bool fallback = false) const {
        const auto value = lookup(key);
        if (!value) {
            return fallback;
        }
        std::string lowered = *value;
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](unsigned char character) {
                           return static_cast<char>(std::tolower(character));
                       });
        if (lowered == "1" || lowered == "true" || lowered == "yes" ||
            lowered == "on") {
            return true;
        }
        if (lowered == "0" || lowered == "false" || lowered == "no" ||
            lowered == "off") {
            return false;
        }
        throw std::runtime_error("invalid boolean for config key " + key);
    }

private:
    [[nodiscard]] std::optional<std::string> lookup(const std::string& key) const {
        const auto iterator = values_.find(key);
        if (iterator != values_.end()) {
            return iterator->second;
        }
        if (const char* environment = std::getenv(environment_name(key).c_str())) {
            return std::string(environment);
        }
        return std::nullopt;
    }

    std::unordered_map<std::string, std::string> values_;
};

} // namespace hft::config
