// Assembly and the main loop.
//
// Owns the order things come up in and the order they come down in: resolver,
// sinks, pipeline, probes, event loop. main() parses arguments and hands over.

#pragma once

#include <csignal>
#include <memory>
#include <vector>

#include "core/config.h"
#include "core/pipeline.h"
#include "core/probe.h"
#include "core/registry.h"
#include "core/sink.h"
#include "runtime/event_loop.h"

namespace slurm_tracer {

class CgroupResolver;

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
    int run(const volatile std::sig_atomic_t& stop);

private:
    void start_sinks();
    void start_probes();
    void retry_discovery();
    void report_shutdown() const;

    Config config_;
    Registries registries_;

    std::unique_ptr<CgroupResolver> resolver_;
    std::vector<std::unique_ptr<Sink>> sinks_;
    std::vector<std::unique_ptr<Probe>> probes_;
    std::unique_ptr<Pipeline> pipeline_;
    EventLoop loop_;
};

} // namespace slurm_tracer
