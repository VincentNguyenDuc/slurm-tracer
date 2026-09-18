#include "core/config/config_file.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace slurm_tracer {
namespace {

using Json = nlohmann::json;

// Routes a value through ComponentConfig's own parsing rather than
// re-implementing bool/duration parsing here -- there is exactly one
// definition of what "10s" or true means, and it is that one.
ComponentConfig one_shot(const std::string& value) {
    ComponentConfig c;
    c.set("v", value);
    return c;
}

// Converts whatever JSON type a leaf value actually is back to a string --
// ComponentConfig is deliberately string-keyed and string-valued no matter
// what the file format's native types are.
std::string stringify(const Json& v) {
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_boolean())
        return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer() || v.is_number_unsigned())
        return std::to_string(v.get<int64_t>());
    if (v.is_number_float())
        return std::to_string(v.get<double>());
    return v.dump(); // array/object/null: not a value any component expects
}

// Removes any probes/sinks entry explicitly marked `"enabled": false`.
// Presence in the map means enabled, so a section that exists in the file but
// disables itself must not leave an entry behind -- the alternative,
// defaulting a missing `enabled` key to true, is exactly why every *other*
// section this loader creates is left in the map.
void drop_disabled(std::map<std::string, ComponentConfig>& components) {
    for (auto it = components.begin(); it != components.end();) {
        if (it->second.get_bool("enabled", true))
            ++it;
        else
            it = components.erase(it);
    }
}

// Walks a "probes"/"sinks" object into `components`, one ComponentConfig per
// member -- present even for `{}`, which is what turns a component on with no
// settings (presence means enabled).
bool load_components(
    const Json& section,
    const std::string& section_name,
    std::map<std::string, ComponentConfig>& components,
    std::string& error
) {
    for (const auto& [name, value] : section.items()) {
        if (!value.is_object()) {
            error = "\"" + section_name + "." + name + "\" must be an object";
            return false;
        }
        ComponentConfig& c = components[name];
        for (const auto& [key, v] : value.items())
            c.set(key, stringify(v));
    }
    return true;
}

// Walks "node" or "slurm" into the handful of top-level Config fields each
// recognises. `handler` returns false for a key it does not know.
template <typename Handler>
bool load_object(
    const Json& section, const char* section_name, Handler handler, std::string& error
) {
    if (!section.is_object()) {
        error = "\"" + std::string(section_name) + "\" must be an object";
        return false;
    }
    for (const auto& [key, v] : section.items()) {
        if (!handler(key, v)) {
            error = "unknown key \"" + key + "\" in \"" + section_name + "\"";
            return false;
        }
    }
    return true;
}

} // namespace

std::optional<Config> load_config_file(const std::string& path, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot open " + path;
        return std::nullopt;
    }

    Json root;
    try {
        // ignore_comments: plain JSON has no comment syntax, but a config file
        // meant to be hand-edited needs one -- `//` and `/* */` stay legal.
        root = Json::parse(in, /*cb=*/nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
    } catch (const Json::parse_error& e) {
        error = path + ": " + e.what();
        return std::nullopt;
    }

    if (!root.is_object()) {
        error = path + ": top level must be an object";
        return std::nullopt;
    }

    Config config;
    try {
        for (const auto& [section, value] : root.items()) {
            bool ok = true;
            std::string sub_error;

            if (section == "node") {
                ok = load_object(
                    value,
                    "node",
                    [&](const std::string& key, const Json& v) {
                    if (key == "cluster")
                        config.cluster = v.get<std::string>();
                    else if (key == "node")
                        config.node = v.get<std::string>();
                    else if (key == "plugin_dir")
                        config.plugin_dir = v.get<std::string>();
                    else if (key == "batch_size")
                        config.batch_size = static_cast<size_t>(
                            one_shot(stringify(v)).get_uint("v", config.batch_size)
                        );
                    else if (key == "flush_interval")
                        config.flush_interval =
                            one_shot(stringify(v)).get_duration("v", config.flush_interval);
                    else if (key == "verbose")
                        config.verbose = one_shot(stringify(v)).get_bool("v", config.verbose);
                    else
                        return false;
                    return true;
                    },
                    sub_error);
            } else if (section == "slurm") {
                ok = load_object(
                    value,
                    "slurm",
                    [&](const std::string& key, const Json& v) {
                    if (key == "cgroup_root")
                        config.cgroup_root = v.get<std::string>();
                    else
                        return false;
                    return true;
                    },
                    sub_error);
            } else if (section == "probes") {
                if (!value.is_object()) {
                    ok = false;
                    sub_error = "\"probes\" must be an object";
                } else {
                    ok = load_components(value, "probes", config.probes, sub_error);
                }
            } else if (section == "sinks") {
                if (!value.is_object()) {
                    ok = false;
                    sub_error = "\"sinks\" must be an object";
                } else {
                    ok = load_components(value, "sinks", config.sinks, sub_error);
                }
            } else {
                ok = false;
                sub_error = "unknown top-level key \"" + section + "\"";
            }

            if (!ok) {
                error = path + ": " + sub_error;
                return std::nullopt;
            }
        }
    } catch (const Json::exception& e) {
        // A value of the wrong JSON type for the key it's under (e.g. cluster
        // as a number) -- nlohmann's own message already names the key.
        error = path + ": " + e.what();
        return std::nullopt;
    }

    drop_disabled(config.probes);
    drop_disabled(config.sinks);
    return config;
}

} // namespace slurm_tracer
