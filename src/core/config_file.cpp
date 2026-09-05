#include "core/config_file.h"

#include <cstdlib>
#include <fstream>
#include <string>

namespace slurm_tracer {
namespace {

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Drops a trailing '#' comment, but not one inside a quoted string.
std::string strip_comment(const std::string& line) {
    bool in_quotes = false;
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == '"')
            in_quotes = !in_quotes;
        else if (line[i] == '#' && !in_quotes)
            return line.substr(0, i);
    }
    return line;
}

std::string unquote(const std::string& value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        return value.substr(1, value.size() - 2);
    return value;
}

// Routes a value through ComponentConfig's own parsing (core/config.cpp)
// rather than re-implementing bool/duration parsing here -- there is exactly
// one definition of what "10s" or "true" means, and it is that one.
ComponentConfig one_shot(const std::string& value) {
    ComponentConfig c;
    c.set("v", value);
    return c;
}

// Removes any probes/sinks entry explicitly marked `enabled = false`.
// Presence in the map means enabled (core/config.h), so a section that
// exists in the file but disables itself must not leave an entry behind --
// the alternative, defaulting a missing `enabled` key to true, is exactly
// why every *other* section this loader creates is left in the map.
void drop_disabled(std::map<std::string, ComponentConfig>& components) {
    for (auto it = components.begin(); it != components.end();) {
        if (it->second.get_bool("enabled", true))
            ++it;
        else
            it = components.erase(it);
    }
}

} // namespace

std::optional<Config> load_config_file(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return std::nullopt;
    }

    Config config;
    enum class Section { kNone, kNode, kSlurm, kProbe, kSink };
    Section section = Section::kNone;
    std::string component; // valid when section is kProbe/kSink

    std::string raw_line;
    size_t lineno = 0;
    while (std::getline(in, raw_line)) {
        ++lineno;
        const std::string line = trim(strip_comment(raw_line));
        if (line.empty())
            continue;

        auto fail = [&](const std::string& what) -> std::optional<Config> {
            error = path + ":" + std::to_string(lineno) + ": " + what;
            return std::nullopt;
        };

        if (line.front() == '[') {
            if (line.back() != ']')
                return fail("unterminated section header");

            const std::string header = line.substr(1, line.size() - 2);
            if (header == "node") {
                section = Section::kNode;
            } else if (header == "slurm") {
                section = Section::kSlurm;
            } else if (header.compare(0, 7, "probes.") == 0 && header.size() > 7) {
                section = Section::kProbe;
                component = header.substr(7);
                config.probes.try_emplace(component);
            } else if (header.compare(0, 6, "sinks.") == 0 && header.size() > 6) {
                section = Section::kSink;
                component = header.substr(6);
                config.sinks.try_emplace(component);
            } else {
                return fail("unknown section [" + header + "]");
            }
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string::npos)
            return fail("expected 'key = value'");

        const std::string key = trim(line.substr(0, eq));
        const std::string value = unquote(trim(line.substr(eq + 1)));
        if (key.empty())
            return fail("empty key");

        switch (section) {
        case Section::kNone:
            return fail("'" + key + "' outside any [section]");

        case Section::kNode:
            if (key == "cluster")
                config.cluster = value;
            else if (key == "node")
                config.node = value;
            else if (key == "batch_size")
                config.batch_size =
                    static_cast<size_t>(one_shot(value).get_uint("v", config.batch_size));
            else if (key == "flush_interval")
                config.flush_interval = one_shot(value).get_duration("v", config.flush_interval);
            else if (key == "verbose")
                config.verbose = one_shot(value).get_bool("v", config.verbose);
            else
                return fail("unknown key '" + key + "' in [node]");
            break;

        case Section::kSlurm:
            if (key == "cgroup_root")
                config.cgroup_root = value;
            else
                return fail("unknown key '" + key + "' in [slurm]");
            break;

        case Section::kProbe:
            config.probes[component].set(key, value);
            break;

        case Section::kSink:
            config.sinks[component].set(key, value);
            break;
        }
    }

    drop_disabled(config.probes);
    drop_disabled(config.sinks);
    return config;
}

} // namespace slurm_tracer
