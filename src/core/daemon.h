// Assembly and the main loop.
//
// Owns the order things come up in and the order they come down in: resolver,
// cgroup watcher, sinks, pipeline, probes, event loop. main() parses arguments
// and hands over.

#pragma once

#include <csignal>
#include <memory>
#include <vector>

#include "core/attribution/cgroup_watcher.h"
#include "core/config/config.h"
#include "core/event_loop.h"
#include "core/pipeline/pipeline.h"
#include "core/probe/probe.h"
#include "core/registry.h"
#include "core/sink/sink.h"

namespace slurm_tracer {

class Daemon {
public:
    explicit Daemon(Config config);
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
    void add_probe(const std::string& name, const ComponentConfig& config);
    void remove_sink(const std::string& name);
    void add_sink(const std::string& name, const ComponentConfig& config);
    void wire_sinks();
    void remove_probe(const std::string& name);

    Config config_;
    Registries registries_;

    std::unique_ptr<CgroupResolver> resolver_;
    std::unique_ptr<CgroupWatcher> cgroup_watcher_;
    std::vector<std::unique_ptr<Sink>> sinks_;
    std::vector<std::unique_ptr<Probe>> probes_;
    std::unique_ptr<Pipeline> pipeline_;
    EventLoop loop_;
};

} // namespace slurm_tracer
