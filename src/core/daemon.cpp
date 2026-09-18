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

// Stamps the emitting instance's id on every record on its way to the
// pipeline. A probe sets r.probe to its own plugin name, which is all it
// knows; two instances of that plugin would be indistinguishable downstream,
// so the id the config gave this one wins. With one instance per plugin --
// the usual case -- it writes back what the probe already put there.
class InstanceEmitter : public RecordEmitter {
public:
    InstanceEmitter(std::string id, RecordEmitter& out)
        : id_(std::move(id))
        , out_(out) {}

    void emit(Record r) override {
        r.probe = id_;
        out_.emit(std::move(r));
    }

private:
    std::string id_;
    RecordEmitter& out_;
};

} // namespace

Daemon::Daemon(Config config, std::string config_path)
    : config_(std::move(config))
    , config_path_(std::move(config_path))
    , loader_(config_.plugin_dir) {}

Daemon::~Daemon() {
    if (cgroup_watcher_)
        cgroup_watcher_->detach();
    for (const auto& loaded : probes_)
        loaded.probe->detach();
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
    if (config_.plugin_dir.empty()) {
        spdlog::error("no plugin_dir configured; no probe or sink could be loaded");
        return false;
    }

    if (auto root = discover_cgroup_root(config_.cgroup_root)) {
        resolver_ = std::make_unique<CgroupResolver>(*root);
    } else {
        spdlog::error("attribution: no Slurm cgroup root found; refusing to start");
        return false;
    }

    if (!resolver_->start()) {
        spdlog::error("attribution: starting resolver failed");
        return false;
    }

    for (const auto& [id, config] : config_.sinks)
        add_sink(id, config);
    if (sinks_.empty()) {
        spdlog::error("no sinks configured; records would go nowhere");
        return false;
    }

    Pipeline::Options popt;
    popt.node = config_.node;
    popt.cluster = config_.cluster;
    popt.batch_size = config_.batch_size;
    popt.flush_interval = config_.flush_interval;

    // Built with no sinks, then pointed at the ones already loaded above --
    // the same path every later add_sink/remove_sink takes.
    pipeline_ = std::make_unique<Pipeline>(popt, std::vector<Sink*>{}, resolver_.get());
    wire_sinks();

    if (!start_cgroup_watcher())
        return false;

    for (const auto& [id, config] : config_.probes)
        add_probe(id, config);

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
    for (const auto& loaded : sinks_)
        dropped += loaded.sink->dropped();
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

    for (const auto& id : diff->removed_sinks)
        remove_sink(id);
    for (const auto& id : diff->added_sinks)
        add_sink(id, next.sinks.at(id));

    for (const auto& id : diff->removed_probes)
        remove_probe(id);
    for (const auto& id : diff->added_probes)
        add_probe(id, next.probes.at(id));

    config_.sinks = std::move(next.sinks);
    config_.probes = std::move(next.probes);

    if (sinks_.empty())
        spdlog::warn("reload: no sinks configured; records go nowhere until the next reload");
    if (probes_.empty())
        spdlog::warn("reload: no probes running; nothing is being collected");

    spdlog::info("reload: config re-read from {}", config_path_);
}

void Daemon::add_probe(const std::string& id, const ComponentConfig& config) {
    auto probe = loader_.load_probe(id, plugin_of(id, config), config);
    if (!probe) // the loader said why
        return;

    if (!probe->open(config) || !probe->load() || !probe->attach()) {
        spdlog::warn("probe {}: disabled", id);
        probe.reset();
        loader_.unload_probe(id);
        return;
    }

    auto emitter = std::make_unique<InstanceEmitter>(id, *pipeline_);
    if (!loop_.add(*probe, *emitter)) {
        probe->detach();
        probe.reset();
        loader_.unload_probe(id);
        return;
    }

    probes_.push_back(LoadedProbe{id, std::move(probe), std::move(emitter)});
}

void Daemon::remove_sink(const std::string& id) {
    const auto it =
        std::find_if(sinks_.begin(), sinks_.end(), [&](const auto& s) { return s.id == id; });
    if (it == sinks_.end())
        return;
    std::unique_ptr<Sink> doomed = std::move(it->sink);
    sinks_.erase(it);
    wire_sinks();
    doomed->flush();

    // Destroyed explicitly, before its plugin is unmapped: the destructor
    // this runs lives in that mapping. Letting `doomed` fall out of scope
    // instead would run it after the unload below.
    doomed.reset();
    loader_.unload_sink(id);
}

void Daemon::add_sink(const std::string& id, const ComponentConfig& config) {
    auto sink = loader_.load_sink(id, plugin_of(id, config), config);
    if (!sink) // the loader said why
        return;

    sinks_.push_back(LoadedSink{id, std::move(sink)});
    if (pipeline_)
        wire_sinks();
}

void Daemon::wire_sinks() {
    if (!pipeline_)
        return;

    std::vector<Sink*> sink_ptrs;
    sink_ptrs.reserve(sinks_.size());
    for (const auto& loaded : sinks_)
        sink_ptrs.push_back(loaded.sink.get());
    pipeline_->set_sinks(std::move(sink_ptrs));
}

void Daemon::remove_probe(const std::string& id) {
    const auto it =
        std::find_if(probes_.begin(), probes_.end(), [&](const auto& p) { return p.id == id; });
    if (it == probes_.end())
        return;

    std::vector<Probe*> also_dropped = loop_.remove(*it->probe);
    it->probe->detach();
    probes_.erase(it); // destroys the probe, so its plugin can be unmapped
    loader_.unload_probe(id);

    // loop_.remove() can knock out an unrelated probe's ring buffer binding
    // as a side effect of rebuilding the shared one (see EventLoop::remove).
    // Those are just as gone as the one actually being removed here, so they
    // get the same detach-and-forget treatment rather than sitting in
    // probes_ looking alive while producing nothing.
    for (Probe* dead : also_dropped) {
        const auto dead_it = std::find_if(probes_.begin(), probes_.end(), [&](const auto& p) {
            return p.probe.get() == dead;
        });
        if (dead_it == probes_.end())
            continue;
        const std::string dead_id = dead_it->id; // copied before it is erased
        dead_it->probe->detach();
        probes_.erase(dead_it);
        loader_.unload_probe(dead_id);
    }
}

} // namespace slurm_tracer
