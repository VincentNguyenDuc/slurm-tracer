// Assembly and the main loop.
//
// Owns the order things come up in and the order they come down in: resolver,
// cgroup watcher, sinks, pipeline, probes, event loop. main() parses arguments
// and hands over.

#pragma once

#include <csignal>
#include <memory>
#include <string>
#include <vector>

#include "core/attribution/cgroup_watcher.h"
#include "core/config/config.h"
#include "core/event_loop.h"
#include "core/pipeline/pipeline.h"
#include "core/plugin_loader.h"
#include "core/probe/probe.h"
#include "core/sink/sink.h"

namespace slurm_tracer {

class Daemon {
public:
    // config_path is where reload_config() re-reads from on SIGHUP; empty
    // means the process was configured without --config, so reload is a
    // no-op.
    Daemon(Config config, std::string config_path);
    ~Daemon();

    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    // Brings everything up. False means nothing useful could run — no sinks, or
    // no probe that survived load. An individual probe failing is not a start
    // failure; it is disabled and reported.
    bool start();

    // Runs until `stop` becomes non-zero. Returns the process exit code.
    int run(const volatile std::sig_atomic_t& stop, volatile std::sig_atomic_t& reload);

private:
    bool start_cgroup_watcher();
    void report_shutdown() const;

    void reload_config();
    void add_probe(const std::string& id, const ComponentConfig& config);
    void remove_sink(const std::string& id);
    void add_sink(const std::string& id, const ComponentConfig& config);
    void wire_sinks();
    void remove_probe(const std::string& id);

    // What a config entry became. The id is the daemon's, not the plugin's:
    // a plugin's own name() is its type ("http"), which two instances of it
    // share, so identity has to be tracked out here.
    struct LoadedSink {
        std::string id;
        std::unique_ptr<Sink> sink;
    };
    struct LoadedProbe {
        std::string id;
        std::unique_ptr<Probe> probe;
        // Heap-held for a stable address: the event loop keeps a pointer to
        // it, which a vector reallocation must not invalidate.
        std::unique_ptr<RecordEmitter> emitter;
    };

    Config config_;
    std::string config_path_;

    PluginLoader loader_;
    std::unique_ptr<CgroupResolver> resolver_;
    std::unique_ptr<CgroupWatcher> cgroup_watcher_;
    std::vector<LoadedSink> sinks_;
    std::vector<LoadedProbe> probes_;
    std::unique_ptr<Pipeline> pipeline_;
    EventLoop loop_;
};

} // namespace slurm_tracer
