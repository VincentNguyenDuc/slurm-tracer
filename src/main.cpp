// slurm-tracer: eBPF observability for Slurm compute nodes.
//
// The daemon owns no probe and no sink. It builds both from the registry, gives
// each probe a ring buffer, and runs the poll loop; the resolver turns each
// event's cgroup id into a Slurm job/step/task and the pipeline fans records out
// to the sinks. Which plugins exist is decided by the build, not by this file.
// See docs/DESIGN.md.

#include <bpf/libbpf.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "core/attribution.h"
#include "core/pipeline.h"
#include "core/probe.h"
#include "core/registry.h"
#include "core/sink.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

int libbpf_print(enum libbpf_print_level level, const char* fmt, va_list args) {
    if (level == LIBBPF_DEBUG)
        return 0;
    std::vfprintf(stderr, fmt, args);
    return 0;
}

struct Options {
    std::string cluster = "local";
    std::string node;
    std::string cgroup_root; // empty = auto-discover
    size_t batch_size = 64;
    unsigned flush_ms = 1000;
    bool verbose = false;
};

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [options]\n"
              << "  --cluster <name>      cluster name stamped on every record (default: local)\n"
              << "  --node <name>         node name (default: hostname)\n"
              << "  --cgroup-root <path>  Slurm cgroup root; auto-discovered when omitted\n"
              << "  --batch-size <n>      records per batch handed to sinks (default: 64)\n"
              << "  --flush-ms <n>        max ms a partial batch waits (default: 1000)\n"
              << "  --verbose             log every resolver decision\n";
}

bool parse_args(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << arg << " requires " << what << "\n";
                return nullptr;
            }
            return argv[++i];
        };

        if (arg == "--cluster") {
            const char* v = next("a name");
            if (!v)
                return false;
            opt.cluster = v;
        } else if (arg == "--node") {
            const char* v = next("a name");
            if (!v)
                return false;
            opt.node = v;
        } else if (arg == "--cgroup-root") {
            const char* v = next("a path");
            if (!v)
                return false;
            opt.cgroup_root = v;
        } else if (arg == "--batch-size") {
            const char* v = next("a count");
            if (!v)
                return false;
            opt.batch_size = std::strtoul(v, nullptr, 10);
            if (opt.batch_size == 0)
                opt.batch_size = 1;
        } else if (arg == "--flush-ms") {
            const char* v = next("a duration");
            if (!v)
                return false;
            opt.flush_ms = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
        } else if (arg == "--verbose") {
            opt.verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            std::cerr << "unknown option: " << arg << "\n";
            usage(argv[0]);
            return false;
        }
    }
    return true;
}

std::string hostname() {
    char buf[256]{};
    if (::gethostname(buf, sizeof(buf) - 1) != 0)
        return "unknown";
    return buf;
}

// Ring buffer callback. The daemon does not know what the bytes mean; the probe
// that owns the buffer does the translating.
struct EventContext {
    slurm_tracer::Probe* probe;
    slurm_tracer::Pipeline* pipeline;
};

int handle_event(void* raw_ctx, void* data, size_t size) {
    auto& ctx = *static_cast<EventContext*>(raw_ctx);
    ctx.probe->on_event(data, size, *ctx.pipeline);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt))
        return EXIT_FAILURE;
    if (opt.node.empty())
        opt.node = hostname();

    libbpf_set_print(libbpf_print);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // Attribution first: if the cgroup root is wrong we want to say so before
    // loading anything into the kernel.
    std::unique_ptr<slurm_tracer::CgroupResolver> resolver;
    if (auto root = slurm_tracer::discover_cgroup_root(opt.cgroup_root)) {
        resolver = std::make_unique<slurm_tracer::CgroupResolver>(*root);
        if (!resolver->start()) {
            resolver.reset();
        } else {
            std::cerr << "attribution: cgroup root " << *root << ", " << resolver->size()
                      << " cgroups known at startup\n";
        }
    } else {
        // Not fatal. Unattributed data is worth more than no data, and this is
        // exactly the case a rising unattributed rate is meant to surface. The
        // poll loop keeps retrying: on a compute node the tracer usually starts
        // at boot, before slurmd has created the scope directory at all.
        std::cerr << "attribution: no Slurm cgroup root yet; retrying, records unattributed "
                     "until then\n";
    }

    // Probes and sinks both come from the registry, so the daemon never names a
    // concrete one. The registry itself is populated by the generated manifest.
    slurm_tracer::Registries registries;
    slurm_tracer::register_all(registries);

    // Everything compiled into this build runs. Which plugins that is, is a
    // build-time choice (-DST_PLUGINS=...); phase 6 makes it a runtime one, read
    // from configuration.
    const slurm_tracer::ComponentConfig default_config;

    std::vector<std::unique_ptr<slurm_tracer::Sink>> sinks;
    std::vector<slurm_tracer::Sink*> sink_ptrs;
    for (const std::string& name : registries.sinks.names()) {
        if (auto s = registries.sinks.create(name, default_config)) {
            sink_ptrs.push_back(s.get());
            sinks.push_back(std::move(s));
        }
    }
    if (sinks.empty()) {
        std::cerr << "no sinks in this build; nothing would be written\n";
        return EXIT_FAILURE;
    }

    slurm_tracer::Pipeline::Options popt;
    popt.node = opt.node;
    popt.cluster = opt.cluster;
    popt.batch_size = opt.batch_size;
    popt.flush_interval = std::chrono::milliseconds(opt.flush_ms);
    popt.verbose = opt.verbose;

    slurm_tracer::Pipeline pipeline(popt, sink_ptrs);
    pipeline.set_resolver(resolver.get());

    // One ring buffer per probe, per DESIGN §5: a chatty probe must not be able
    // to starve a quiet one. libbpf polls them all through a single epoll set.
    std::vector<std::unique_ptr<slurm_tracer::Probe>> probes;
    std::vector<std::unique_ptr<EventContext>> contexts; // stable addresses for libbpf
    ring_buffer* rb = nullptr;

    for (const std::string& name : registries.probes.names()) {
        auto probe = registries.probes.create(name, default_config);
        if (!probe)
            continue;

        // A probe that cannot load is disabled on its own; the daemon keeps
        // running with the rest. Clusters are heterogeneous, and a node with an
        // older kernel should lose one probe rather than all observability.
        if (!probe->open(default_config) || !probe->load() || !probe->attach()) {
            std::cerr << "probe " << name << ": disabled\n";
            continue;
        }

        const int fd = probe->ring_fd();
        if (fd >= 0) {
            contexts.push_back(std::unique_ptr<EventContext>(new EventContext{
                probe.get(), &pipeline}));
            void* ctx = contexts.back().get();

            const bool ok = rb == nullptr
                                ? (rb = ring_buffer__new(fd, handle_event, ctx, nullptr)) != nullptr
                                : ring_buffer__add(rb, fd, handle_event, ctx) == 0;
            if (!ok) {
                std::cerr << "probe " << name << ": failed to set up ring buffer, disabled\n";
                contexts.pop_back();
                probe->detach();
                continue;
            }
        }

        probes.push_back(std::move(probe));
    }

    if (probes.empty()) {
        std::cerr << "no probes running; nothing to collect\n";
        return EXIT_FAILURE;
    }

    std::cerr << "attached; streaming records for cluster=" << opt.cluster << " node=" << opt.node
              << " (Ctrl-C to stop)\n";

    int rc = EXIT_SUCCESS;
    auto last_discovery = std::chrono::steady_clock::now();
    while (!g_stop) {
        // rb is null when every running probe aggregates in-kernel rather than
        // streaming events; the loop still has to tick and flush.
        if (rb != nullptr) {
            const int err = ring_buffer__poll(rb, 100 /* ms */);
            if (err < 0 && err != -EINTR) {
                std::cerr << "ring buffer poll failed: " << err << "\n";
                rc = EXIT_FAILURE;
                break;
            }
        }

        if (resolver)
            resolver->tick();

        const auto now = std::chrono::steady_clock::now();

        // slurmd may not have created the cgroup scope when we started. Keep
        // looking, so a node that boots the tracer first still attributes.
        if (!resolver &&
            std::chrono::duration_cast<std::chrono::seconds>(now - last_discovery).count() >= 5) {
            last_discovery = now;
            if (auto root = slurm_tracer::discover_cgroup_root(opt.cgroup_root)) {
                auto candidate = std::make_unique<slurm_tracer::CgroupResolver>(*root);
                if (candidate->start()) {
                    std::cerr << "attribution: cgroup root appeared at " << *root << ", "
                              << candidate->size() << " cgroups known\n";
                    resolver = std::move(candidate);
                    pipeline.set_resolver(resolver.get());
                }
            }
        }

        // A partial batch must not sit indefinitely on a quiet node.
        pipeline.tick(now);
    }

    pipeline.flush();

    std::cerr << "shutting down: " << pipeline.stats().records << " events";
    if (resolver) {
        const auto& s = resolver->stats();
        std::cerr << ", attribution hits=" << s.hits << " misses=" << s.misses
                  << " stale=" << s.stale << " rescans=" << s.rescans;
    }
    uint64_t dropped = 0;
    for (const auto& s : sinks)
        dropped += s->dropped();
    std::cerr << ", dropped batches=" << dropped << "\n";

    if (rb != nullptr)
        ring_buffer__free(rb);
    for (const auto& p : probes)
        p->detach();
    return rc;
}
