#include "daemon.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <utility>

#include "core/attribution.h"

namespace slurm_tracer {
namespace {

constexpr auto kPollTimeout = std::chrono::milliseconds(100);

// Which probes/sinks to run: what the config names, or everything this build
// contains when it names none. A config that lists probes/sinks is authoritative,
// including about the ones it leaves out.
template <typename T>
std::vector<std::pair<std::string, ComponentConfig>> selected(
    const std::map<std::string, ComponentConfig>& configured, const Registry<T>& registry
) {
    std::vector<std::pair<std::string, ComponentConfig>> out;
    if (!configured.empty()) {
        for (const auto& [name, config] : configured)
            out.emplace_back(name, config);
        return out;
    }
    for (const std::string& name : registry.names())
        out.emplace_back(name, ComponentConfig{});
    return out;
}

} // namespace

Daemon::Daemon(Config config)
    : config_(std::move(config)) {
    register_all(registries_);
}

Daemon::~Daemon() {
    for (const auto& probe : probes_)
        probe->detach();
}

void Daemon::start_sinks() {
    for (const auto& [name, config] : selected(config_.sinks, registries_.sinks)) {
        auto sink = registries_.sinks.create(name, config);
        if (!sink) {
            spdlog::warn("sink {}: not in this build, skipped", name);
            continue;
        }
        sinks_.push_back(std::move(sink));
    }
}

void Daemon::start_probes() {
    for (const auto& [name, config] : selected(config_.probes, registries_.probes)) {
        auto probe = registries_.probes.create(name, config);
        if (!probe) {
            spdlog::warn("probe {}: not in this build, skipped", name);
            continue;
        }

        // Most probes ignore this; the one that is itself the attribution
        // mechanism (cgroup_lifecycle) uses it instead of RecordEmitter.
        if (resolver_)
            probe->bind_resolver(*resolver_);

        // Failure isolation, per DESIGN §5. A probe that cannot load — missing
        // tracepoint, verifier rejection, kernel too old — is disabled and the
        // daemon keeps running with the rest. Clusters are heterogeneous; a node
        // with an older kernel should lose one probe, not all observability.
        if (!probe->open(config) || !probe->load() || !probe->attach()) {
            if (probe->critical()) {
                spdlog::warn(
                    "probe {}: disabled -- this is the attribution mechanism, every "
                    "record will now be unattributed",
                    name
                );
            } else {
                spdlog::warn("probe {}: disabled", name);
            }
            continue;
        }
        if (!loop_.add(*probe, *pipeline_)) {
            probe->detach();
            continue;
        }
        probes_.push_back(std::move(probe));
    }
}

bool Daemon::start() {
    // Attribution first: if the cgroup root is wrong we want to say so before
    // loading anything into the kernel.
    if (auto root = discover_cgroup_root(config_.cgroup_root)) {
        auto candidate = std::make_unique<CgroupResolver>(*root);
        if (candidate->start()) {
            spdlog::info(
                "attribution: cgroup root {}, {} cgroups known at startup", *root, candidate->size()
            );
            resolver_ = std::move(candidate);
        }
    } else {
        // Fatal, not degraded: run() below exits as soon as it sees resolver_
        // still null. The deployment is responsible for not starting this
        // daemon before slurmd has created the cgroup scope -- see
        // entrypoint-worker.sh's wait_for_cgroup_root.
        spdlog::error("attribution: no Slurm cgroup root found; refusing to start");
    }

    start_sinks();
    if (sinks_.empty()) {
        spdlog::error("no sinks configured; records would go nowhere");
        return false;
    }

    std::vector<Sink*> sink_ptrs;
    sink_ptrs.reserve(sinks_.size());
    for (const auto& sink : sinks_)
        sink_ptrs.push_back(sink.get());

    Pipeline::Options popt;
    popt.node = config_.node;
    popt.cluster = config_.cluster;
    popt.batch_size = config_.batch_size;
    popt.flush_interval = config_.flush_interval;

    pipeline_ = std::make_unique<Pipeline>(popt, sink_ptrs);
    pipeline_->set_resolver(resolver_.get());

    start_probes();
    if (probes_.empty()) {
        spdlog::error("no probes running; nothing to collect");
        return false;
    }
    return true;
}

int Daemon::run(const volatile std::sig_atomic_t& stop) {
    spdlog::info(
        "attached; streaming records for cluster={} node={} (Ctrl-C to stop)",
        config_.cluster,
        config_.node
    );

    int rc = EXIT_SUCCESS;

    while (stop == 0) {
        if (!loop_.poll(kPollTimeout)) {
            rc = EXIT_FAILURE;
            break;
        }

        if (!resolver_) {
            rc = EXIT_FAILURE;
            break;
        }

        resolver_->tick();

        // A partial batch must not sit indefinitely on a quiet node.
        pipeline_->tick(std::chrono::steady_clock::now());
    }

    pipeline_->flush();
    report_shutdown();
    return rc;
}

void Daemon::report_shutdown() const {
    std::string msg = fmt::format("shutting down: {} events", pipeline_->stats().records);
    if (resolver_) {
        const auto& s = resolver_->stats();
        msg += fmt::format(
            ", attribution hits={} misses={} stale={} rescans={}",
            s.hits,
            s.misses,
            s.stale,
            s.rescans
        );
    }

    uint64_t dropped = 0;
    for (const auto& sink : sinks_)
        dropped += sink->dropped();
    msg += fmt::format(", dropped batches={}", dropped);
    spdlog::info(msg);
}

} // namespace slurm_tracer
