// slurm-tracer: eBPF observability for Slurm compute nodes.
//
// M1: the proc_lifecycle probe streams exec/exit events out of a BPF ring
// buffer; the resolver turns each event's cgroup id into a Slurm job/step/task;
// the result is batched into slurm_tracer::Record and fanned out to the configured sinks.
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
#include "core/record.h"
#include "core/registry.h"
#include "core/sink.h"
#include "proc_lifecycle.skel.h"
// Phase 1 leaves the probe wired directly into the daemon; phase 5 moves this
// translation into the probe itself and this include goes away.
#include "plugins/probes/proc_lifecycle/proc_lifecycle_events.h"

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

// Translates one proc_lifecycle event into a record.
//
// Everything here is knowledge only this probe has: its own wire struct, its own
// hdr.type values, and how the kernel packs an exit code. Attribution, the
// uid->user lookup, the wall-clock conversion and the node/cluster stamp are the
// pipeline's job. Phase 5 moves this function into the probe itself.
bool translate(const st_proc_event& e, slurm_tracer::Record& r) {
    switch (e.hdr.type) {
    case ST_EVENT_EXEC:
        r.event_type = "exec";
        r.metric = "proc.exec";
        r.value = 1.0;
        r.unit = "count";
        break;
    case ST_EVENT_EXIT:
        r.event_type = "exit";
        r.metric = "proc.exit";
        // The kernel's exit_code packs the wait(2) status: high byte is the
        // exit status, low 7 bits the terminating signal.
        r.value = static_cast<double>((e.exit_code >> 8) & 0xff);
        r.unit = "exit_status";
        if (const int sig = e.exit_code & 0x7f; sig != 0)
            r.attrs.emplace_back("signal", std::to_string(sig));
        break;
    default:
        std::cerr << "unknown event type " << e.hdr.type << "\n";
        return false;
    }

    r.probe = "proc_lifecycle";
    r.ts_ns = e.hdr.ts_ns; // monotonic; the pipeline converts to wall clock
    r.cgroup_id = e.hdr.cgroup_id;
    r.uid = e.hdr.uid;
    r.pid = e.hdr.pid;
    r.tid = e.hdr.tid;
    r.comm.assign(e.comm, ::strnlen(e.comm, ST_COMM_LEN));
    return true;
}

int handle_event(void* raw_ctx, void* data, size_t size) {
    auto& pipeline = *static_cast<slurm_tracer::Pipeline*>(raw_ctx);

    if (size < sizeof(st_proc_event)) {
        std::cerr << "short event: " << size << " bytes\n";
        return 0;
    }

    slurm_tracer::Record r;
    if (!translate(*static_cast<const st_proc_event*>(data), r))
        return 0;

    pipeline.emit(std::move(r));
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

    proc_lifecycle* skel = proc_lifecycle__open();
    if (!skel) {
        std::cerr << "failed to open BPF skeleton\n";
        return EXIT_FAILURE;
    }
    if (proc_lifecycle__load(skel)) {
        std::cerr << "failed to load BPF programs (need CAP_BPF and CAP_PERFMON)\n";
        proc_lifecycle__destroy(skel);
        return EXIT_FAILURE;
    }
    if (proc_lifecycle__attach(skel)) {
        std::cerr << "failed to attach BPF programs\n";
        proc_lifecycle__destroy(skel);
        return EXIT_FAILURE;
    }

    // Sinks come from the registry, so the daemon never names a concrete one.
    // The registry itself is populated by the generated manifest.
    slurm_tracer::Registries registries;
    slurm_tracer::register_all(registries);

    const std::string sink_name = "stdout_json";
    std::unique_ptr<slurm_tracer::Sink> sink =
        registries.sinks.create(sink_name, slurm_tracer::ComponentConfig{});
    if (!sink) {
        std::cerr << "no sink named " << sink_name << " in this build; have:";
        for (const std::string& n : registries.sinks.names())
            std::cerr << ' ' << n;
        std::cerr << "\n";
        proc_lifecycle__destroy(skel);
        return EXIT_FAILURE;
    }

    slurm_tracer::Pipeline::Options popt;
    popt.node = opt.node;
    popt.cluster = opt.cluster;
    popt.batch_size = opt.batch_size;
    popt.flush_interval = std::chrono::milliseconds(opt.flush_ms);
    popt.verbose = opt.verbose;

    slurm_tracer::Pipeline pipeline(popt, {sink.get()});
    pipeline.set_resolver(resolver.get());

    ring_buffer* rb =
        ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event, &pipeline, nullptr);
    if (!rb) {
        std::cerr << "failed to create ring buffer\n";
        proc_lifecycle__destroy(skel);
        return EXIT_FAILURE;
    }

    std::cerr << "attached; streaming records for cluster=" << opt.cluster << " node=" << opt.node
              << " (Ctrl-C to stop)\n";

    int rc = EXIT_SUCCESS;
    auto last_discovery = std::chrono::steady_clock::now();
    while (!g_stop) {
        const int err = ring_buffer__poll(rb, 100 /* ms */);
        if (err < 0 && err != -EINTR) {
            std::cerr << "ring buffer poll failed: " << err << "\n";
            rc = EXIT_FAILURE;
            break;
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
    std::cerr << ", dropped batches=" << sink->dropped() << "\n";

    ring_buffer__free(rb);
    proc_lifecycle__destroy(skel);
    return rc;
}
