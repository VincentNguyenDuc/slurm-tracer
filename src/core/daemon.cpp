#include "daemon.h"

#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include "core/attribution/attribution.h"
#include "core/config/config_file.h"

namespace slurm_tracer {
namespace {

constexpr auto kPollTimeout = std::chrono::milliseconds(100);

} // namespace

Daemon::Daemon(Config config, std::string config_path)
    : config_(std::move(config))
    , config_path_(std::move(config_path)) {
    register_all(registries_);
}

Daemon::~Daemon() {
    if (cgroup_watcher_)
        cgroup_watcher_->detach();
    for (const auto& probe : probes_)
        probe->detach();
}

bool Daemon::start_cgroup_watcher() {
    cgroup_watcher_ = std::make_unique<CgroupWatcher>(*resolver_);

    // Not failure-isolated the way an ordinary probe is: losing this feed
    // means every other probe's records go unattributed, not just one
    // metric, so it gets its own explicit warning instead of start_probes()'s
    // generic "disabled". The daemon still keeps running -- resolver_ falls
    // back to whatever it saw in its startup scan, which is degraded, not
    // fatal.
    if (!cgroup_watcher_->open({}) || !cgroup_watcher_->load() || !cgroup_watcher_->attach()) {
        spdlog::warn("cgroup watcher: failed to attach");
        cgroup_watcher_.reset();
        return false;
    }
    if (!loop_.add(*cgroup_watcher_, *pipeline_)) {
        cgroup_watcher_->detach();
        cgroup_watcher_.reset();
        return false;
    }

    return true;
}

bool Daemon::start() {

    if (auto root = discover_cgroup_root(config_.cgroup_root)) {
        resolver_ = std::make_unique<CgroupResolver>(*root);
        spdlog::info(
            "attribution: cgroup root {}, {} cgroups known at startup", *root, resolver_->size()
        );
    } else {
        spdlog::error("attribution: no Slurm cgroup root found; refusing to start");
        return false;
    }

    if (!resolver_->start()) {
        spdlog::error("attribution: starting resolver failed");
        return false;
    }

    for (const auto& [name, config] : config_.sinks)
        add_sink(name, config);
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

    pipeline_ = std::make_unique<Pipeline>(popt, sink_ptrs, resolver_.get());
    if (!start_cgroup_watcher())
        return false;

    for (const auto& [name, config] : config_.probes)
        add_probe(name, config);

    if (probes_.empty()) {
        spdlog::error("no probes running; nothing to collect");
        return false;
    }
    return true;
}

int Daemon::run(const volatile std::sig_atomic_t& stop, volatile std::sig_atomic_t& reload) {
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

        resolver_->tick();
        pipeline_->tick(std::chrono::steady_clock::now());

        if (reload != 0) {
            reload = 0;
            reload_config();
        }
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

void Daemon::reload_config() {
    if (config_path_.empty()) {
        spdlog::warn("reload: no --config file was loaded at startup; ignoring SIGHUP");
        return;
    }

    std::string error;
    auto loaded = load_config_file(config_path_, error);
    if (!loaded) {
        spdlog::error("reload: {}; keeping current configuration", error);
        return;
    }
    Config next = std::move(*loaded);

    // Decides whether this file can be applied live at all,
    // and which probes/sinks would need to change if so;
    // applying that diff with this daemon's own registries
    // and component lifecycle is all that's left to do here.
    auto diff = diff_for_reload(config_, next);
    if (!diff) {
        spdlog::error(
            "reload: {} cannot be applied without a restart; ignoring this reload", config_path_
        );
        return;
    }

    for (const auto& name : diff->removed_sinks)
        remove_sink(name);
    for (const auto& name : diff->added_sinks)
        add_sink(name, next.sinks.at(name));

    for (const auto& name : diff->removed_probes)
        remove_probe(name);
    for (const auto& name : diff->added_probes)
        add_probe(name, next.probes.at(name));

    config_.sinks = std::move(next.sinks);
    config_.probes = std::move(next.probes);

    if (sinks_.empty())
        spdlog::warn("reload: no sinks configured; records go nowhere until the next reload");
    if (probes_.empty())
        spdlog::warn("reload: no probes running; nothing is being collected");

    spdlog::info("reload: config re-read from {}", config_path_);
}

void Daemon::add_probe(const std::string& name, const ComponentConfig& config) {
    auto probe = registries_.probes.create(name, config);
    if (!probe) {
        spdlog::warn("probe {}: not in this build, skipped", name);
        return;
    }

    if (!probe->open(config) || !probe->load() || !probe->attach()) {
        spdlog::warn("probe {}: disabled", name);
        return;
    }
    if (!loop_.add(*probe, *pipeline_)) {
        probe->detach();
        return;
    }

    probes_.push_back(std::move(probe));
    return;
}

void Daemon::remove_sink(const std::string& name) {
    const auto it = std::find_if(sinks_.begin(), sinks_.end(), [&](const auto& s) {
        return s->name() == name;
    });
    if (it == sinks_.end())
        return;
    std::unique_ptr<Sink> doomed = std::move(*it);
    sinks_.erase(it);
    wire_sinks();
    doomed->flush();
}

void Daemon::add_sink(const std::string& name, const ComponentConfig& config) {
    auto sink = registries_.sinks.create(name, config);
    if (!sink) {
        spdlog::warn("sink {}: not in this build, skipped", name);
        return;
    }
    sinks_.push_back(std::move(sink));
    if (pipeline_)
        wire_sinks();
}

void Daemon::wire_sinks() {
    if (!pipeline_)
        return;

    std::vector<Sink*> sink_ptrs;
    sink_ptrs.reserve(sinks_.size());
    for (const auto& sink : sinks_)
        sink_ptrs.push_back(sink.get());
    pipeline_->set_sinks(std::move(sink_ptrs));
}

void Daemon::remove_probe(const std::string& name) {
    const auto it = std::find_if(probes_.begin(), probes_.end(), [&](const auto& p) {
        return p->name() == name;
    });
    if (it == probes_.end())
        return;
    loop_.remove(**it);
    (*it)->detach();
    probes_.erase(it);
}

} // namespace slurm_tracer
