#include "core/config.h"

#include <cctype>
#include <cstdlib>

namespace slurm_tracer {
namespace {

// "10s" / "500ms" / "2m" / "1500" (bare = milliseconds).
bool parse_duration_ms(const std::string& text, std::chrono::milliseconds& out) {
    if (text.empty())
        return false;

    size_t i = 0;
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) != 0))
        ++i;
    if (i == 0)
        return false;

    errno = 0;
    const unsigned long long value = std::strtoull(text.substr(0, i).c_str(), nullptr, 10);
    if (errno != 0)
        return false;

    const std::string unit = text.substr(i);
    if (unit.empty() || unit == "ms")
        out = std::chrono::milliseconds(value);
    else if (unit == "s")
        out = std::chrono::seconds(value);
    else if (unit == "m")
        out = std::chrono::minutes(value);
    else if (unit == "h")
        out = std::chrono::hours(value);
    else
        return false;
    return true;
}

} // namespace

void ComponentConfig::set(std::string key, std::string value) {
    values_[std::move(key)] = std::move(value);
}

bool ComponentConfig::has(const std::string& key) const { return values_.count(key) != 0; }

std::string ComponentConfig::get(const std::string& key, std::string fallback) const {
    const auto it = values_.find(key);
    return it == values_.end() ? std::move(fallback) : it->second;
}

bool ComponentConfig::get_bool(const std::string& key, bool fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end())
        return fallback;

    std::string v = it->second;
    for (char& c : v)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (v == "true" || v == "yes" || v == "1" || v == "on")
        return true;
    if (v == "false" || v == "no" || v == "0" || v == "off")
        return false;
    return fallback;
}

uint64_t ComponentConfig::get_uint(const std::string& key, uint64_t fallback) const {
    const auto it = values_.find(key);
    if (it == values_.end() || it->second.empty())
        return fallback;

    for (const char c : it->second) {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0)
            return fallback;
    }
    errno = 0;
    const unsigned long long value = std::strtoull(it->second.c_str(), nullptr, 10);
    return errno == 0 ? static_cast<uint64_t>(value) : fallback;
}

std::chrono::milliseconds ComponentConfig::get_duration(
    const std::string& key, std::chrono::milliseconds fallback
) const {
    const auto it = values_.find(key);
    if (it == values_.end())
        return fallback;

    std::chrono::milliseconds parsed{};
    return parse_duration_ms(it->second, parsed) ? parsed : fallback;
}

} // namespace slurm_tracer
