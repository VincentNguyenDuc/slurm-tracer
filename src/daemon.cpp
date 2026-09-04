#include "daemon.h"

#include <chrono>
#include <iostream>
#include <utility>

#include "core/attribution.h"

namespace slurm_tracer {
namespace {

constexpr auto kPollTimeout = std::chrono::milliseconds(100);
constexpr auto kDiscoveryRetry = std::chrono::seconds(5);

// Which plugins to run: what the config names, or everything this build
// contains when it names none. A config that lists plugins is authoritative,
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
            std::cerr << "sink " << name << ": not in this build, skipped\n";
            continue;
        }
        sinks_.push_back(std::move(sink));
    }
}

void Daemon::start_probes() {
    for (const auto& [name, config] : selected(config_.probes, registries_.probes)) {
        auto probe = registries_.probes.create(name, config);
        if (!probe) {
            std::cerr << "probe " << name << ": not in this build, skipped\n";
            continue;
        }

        // Failure isolation, per DESIGN §5. A probe that cannot load — missing
        // tracepoint, verifier rejection, kernel too old — is disabled and the
        // daemon keeps running with the rest. Clusters are heterogeneous; a node
        // with an older kernel should lose one probe, not all observability.
        if (!probe->open(config) || !probe->load() || !probe->attach()) {
            std::cerr << "probe " << name << ": disabled\n";
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
            std::cerr << "attribution: cgroup root " << *root << ", " << candidate->size()
                      << " cgroups known at startup\n";
            resolver_ = std::move(candidate);
        }
    } else {
        // Not fatal. Unattributed data is worth more than no data, and this is
        // exactly the case a rising unattributed rate is meant to surface. The
        // poll loop keeps retrying: on a compute node the tracer usually starts
        // at boot, before slurmd has created the scope directory at all.
        std::cerr << "attribution: no Slurm cgroup root yet; retrying, records unattributed "
                     "until then\n";
    }

    start_sinks();
    if (sinks_.empty()) {
        std::cerr << "no sinks configured; records would go nowhere\n";
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
    popt.verbose = config_.verbose;

    pipeline_ = std::make_unique<Pipeline>(popt, sink_ptrs);
    pipeline_->set_resolver(resolver_.get());

    start_probes();
    if (probes_.empty()) {
        std::cerr << "no probes running; nothing to collect\n";
        return false;
    }
    return true;
}

// slurmd may not have created the cgroup scope when we started. Keep looking, so
// a node that boots the tracer first still attributes.
void Daemon::retry_discovery() {
    auto root = discover_cgroup_root(config_.cgroup_root);
    if (!root)
        return;

    auto candidate = std::make_unique<CgroupResolver>(*root);
    if (!candidate->start())
        return;

    std::cerr << "attribution: cgroup root appeared at " << *root << ", " << candidate->size()
              << " cgroups known\n";
    resolver_ = std::move(candidate);
    pipeline_->set_resolver(resolver_.get());
}

int Daemon::run(const volatile std::sig_atomic_t& stop) {
    std::cerr << "attached; streaming records for cluster=" << config_.cluster
              << " node=" << config_.node << " (Ctrl-C to stop)\n";

    int rc = EXIT_SUCCESS;
    auto last_discovery = std::chrono::steady_clock::now();

    while (stop == 0) {
        if (!loop_.poll(kPollTimeout)) {
            rc = EXIT_FAILURE;
            break;
        }

        if (resolver_)
            resolver_->tick();

        const auto now = std::chrono::steady_clock::now();
        if (!resolver_ && now - last_discovery >= kDiscoveryRetry) {
            last_discovery = now;
            retry_discovery();
        }

        // A partial batch must not sit indefinitely on a quiet node.
        pipeline_->tick(now);
    }

    pipeline_->flush();
    report_shutdown();
    return rc;
}

void Daemon::report_shutdown() const {
    std::cerr << "shutting down: " << pipeline_->stats().records << " events";
    if (resolver_) {
        const auto& s = resolver_->stats();
        std::cerr << ", attribution hits=" << s.hits << " misses=" << s.misses
                  << " stale=" << s.stale << " rescans=" << s.rescans;
    }

    uint64_t dropped = 0;
    for (const auto& sink : sinks_)
        dropped += sink->dropped();
    std::cerr << ", dropped batches=" << dropped << "\n";
}

} // namespace slurm_tracer
