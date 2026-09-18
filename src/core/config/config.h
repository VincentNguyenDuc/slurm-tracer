// Configuration.
//
// Deliberately a plain struct with string-keyed per-component sections rather
// than anything that knows about the config file format. A plugin receives a
// ComponentConfig and nothing else, so the file format stays an implementation
// detail of the loader and adding one never touches the plugin contract.

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace slurm_tracer {

// Settings for one probe or one sink: "probes": {"proc_lifecycle": {"some_key": "value"}}.
//
// Values are kept as strings and converted on read. A component asks for what
// it understands and supplies its own fallback, so an unknown key is inert
// rather than a startup failure — which matters on a heterogeneous cluster
// where one config is pushed to nodes running different versions.
class ComponentConfig {
public:
    void set(std::string key, std::string value);
    bool has(const std::string& key) const;

    std::string get(const std::string& key, std::string fallback = {}) const;
    bool get_bool(const std::string& key, bool fallback) const;
    uint64_t get_uint(const std::string& key, uint64_t fallback) const;

    // Accepts "10s", "500ms", "2m", or a bare number of milliseconds.
    std::chrono::milliseconds get_duration(
        const std::string& key, std::chrono::milliseconds fallback
    ) const;

private:
    std::map<std::string, std::string> values_;
};

struct Config {
    // Stamped on every record.
    std::string cluster = "local";
    std::string node; // empty = use the hostname

    std::string cgroup_root; // empty = auto-discover

    // Where probe/sink .so files are found (core/plugin_loader.h). No
    // default: nothing can be loaded without it, so it is a config error
    // rather than a guess at someone's install layout.
    std::string plugin_dir;

    size_t batch_size = 64;
    std::chrono::milliseconds flush_interval{1000};
    bool verbose = false;

    // Keyed by instance id, not by plugin: two entries can run the same
    // plugin with different settings ("http_primary" and "http_backup", each
    // with its own endpoint), and the id is what names that one instance
    // everywhere else -- in a reload diff, in a log line, in the `probe`
    // field of the records a probe instance produces.
    //
    // Presence means enabled: an id absent from these maps is never
    // constructed, so a node opts in rather than opting out.
    std::map<std::string, ComponentConfig> probes;
    std::map<std::string, ComponentConfig> sinks;
};

// Which plugin an instance is built from: its "plugin" setting, or its own id
// when it does not name one -- so the common case of one instance per plugin
// stays `"oom": {}` with nothing extra to say.
std::string plugin_of(const std::string& id, const ComponentConfig& config);

// Probe/sink instance ids to add or remove to take a running daemon from
// `current` to `next`. An id present in both is left alone, even if its
// ComponentConfig changed -- see diff_for_reload.
struct ConfigDiff {
    std::vector<std::string> added_probes;
    std::vector<std::string> removed_probes;
    std::vector<std::string> added_sinks;
    std::vector<std::string> removed_sinks;
};

// What a running daemon would have to do to move from `current` to `next`
// without a restart, or nullopt if `next` cannot be applied live at all.
//
// Only probes and sinks have a live-update path, by instance id: whatever
// dropped out is removed, whatever is new is added. An id present in both is
// left running as-is -- picking up changed settings for a running component
// would need every plugin to support reconfiguring in place, which none do.
//
// The one exception is an id whose "plugin" changed: it now names different
// code, so leaving the old one running would ignore the only setting whose
// whole job is choosing what to run. That id is removed and added back.
//
// Everything else -- node, cluster, cgroup_root, plugin_dir, batch_size,
// flush_interval, verbose -- has no live-update path at all (node/cluster are
// stamped on every record by the already-running pipeline,
// batch_size/flush_interval are baked into it at construction, cgroup_root
// only matters at the one-time startup scan, and plugin_dir is where the
// already-loaded plugins came from), so a `next` that changes any of those is
// rejected outright rather than partially applied.
std::optional<ConfigDiff> diff_for_reload(const Config& current, const Config& next);

} // namespace slurm_tracer
