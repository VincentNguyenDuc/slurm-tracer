// slurm-tracer: eBPF observability for Slurm compute nodes.
//
// M1: the proc_lifecycle probe streams exec/exit events out of a BPF ring
// buffer; the resolver turns each event's cgroup id into a Slurm job/step/task;
// the result is batched into slurm_tracer::Record and fanned out to the configured sinks.
// See docs/DESIGN.md.

#include <bpf/libbpf.h>
#include <pwd.h>
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
#include <unordered_map>
#include <vector>

#include "core/attribution.h"
#include "core/clock.h"
#include "core/record.h"
#include "core/stdout_json_sink.h"
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

// uid -> user name. Cached: getpwuid_r can hit NSS/LDAP, which has no business
// running once per event.
const std::string* lookup_user(uint32_t uid) {
    static std::unordered_map<uint32_t, std::string> cache;
    auto it = cache.find(uid);
    if (it != cache.end())
        return it->second.empty() ? nullptr : &it->second;

    std::string name;
    passwd pw{};
    passwd* result = nullptr;
    char buf[4096];
    if (::getpwuid_r(uid, &pw, buf, sizeof(buf), &result) == 0 && result && result->pw_name)
        name = result->pw_name;

    auto [inserted, _] = cache.emplace(uid, std::move(name));
    return inserted->second.empty() ? nullptr : &inserted->second;
}

// Everything the ring buffer callback needs.
struct Context {
    const Options* opt;
    slurm_tracer::CgroupResolver* resolver;
    std::vector<slurm_tracer::Sink*>* sinks;
    std::vector<slurm_tracer::Record>* batch;
    // BPF stamps CLOCK_MONOTONIC; records carry wall clock so they join against
    // Slurm's accounting database. One offset, sampled at startup.
    uint64_t boot_offset_ns;
    uint64_t events = 0;
};

void ship(Context& ctx) {
    if (ctx.batch->empty())
        return;
    std::vector<slurm_tracer::Record> batch;
    batch.swap(*ctx.batch);
    // Every sink gets its own copy; a sink may outlive this call and must not
    // alias another sink's records.
    for (size_t i = 0; i < ctx.sinks->size(); ++i) {
        if (i + 1 == ctx.sinks->size())
            (*ctx.sinks)[i]->write(std::move(batch));
        else
            (*ctx.sinks)[i]->write(batch);
    }
}

int handle_event(void* raw_ctx, void* data, size_t size) {
    auto& ctx = *static_cast<Context*>(raw_ctx);

    if (size < sizeof(st_proc_event)) {
        std::cerr << "short event: " << size << " bytes\n";
        return 0;
    }
    const auto* e = static_cast<const st_proc_event*>(data);
    ++ctx.events;

    slurm_tracer::Record r;
    r.ts_ns = e->hdr.ts_ns + ctx.boot_offset_ns;
    r.node = ctx.opt->node;
    r.cluster = ctx.opt->cluster;
    r.probe = "proc_lifecycle";

    r.uid = e->hdr.uid;
    if (const std::string* user = lookup_user(e->hdr.uid))
        r.user = *user;

    if (ctx.resolver) {
        if (auto attr = ctx.resolver->resolve(e->hdr.cgroup_id, e->hdr.ts_ns)) {
            r.job_id = attr->job_id;
            if (!attr->step_id.empty())
                r.step_id = attr->step_id;
            r.task_id = attr->task_id;
        } else if (ctx.opt->verbose) {
            std::cerr << "unattributed cgroup " << e->hdr.cgroup_id << "\n";
        }
    }

    switch (e->hdr.type) {
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
        r.value = static_cast<double>((e->exit_code >> 8) & 0xff);
        r.unit = "exit_status";
        if (const int sig = e->exit_code & 0x7f; sig != 0)
            r.attrs.emplace_back("signal", std::to_string(sig));
        break;
    default:
        std::cerr << "unknown event type " << e->hdr.type << "\n";
        return 0;
    }

    r.pid = e->hdr.pid;
    r.tid = e->hdr.tid;
    r.comm.assign(e->comm, ::strnlen(e->comm, ST_COMM_LEN));
    r.attrs.emplace_back("cgroup_id", std::to_string(e->hdr.cgroup_id));

    ctx.batch->push_back(std::move(r));
    if (ctx.batch->size() >= ctx.opt->batch_size)
        ship(ctx);
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

    slurm_tracer::StdoutJsonSink stdout_sink;
    std::vector<slurm_tracer::Sink*> sinks{&stdout_sink};
    std::vector<slurm_tracer::Record> batch;
    batch.reserve(opt.batch_size);

    Context ctx{};
    ctx.opt = &opt;
    ctx.resolver = resolver.get();
    ctx.sinks = &sinks;
    ctx.batch = &batch;
    ctx.boot_offset_ns = slurm_tracer::boot_offset_ns();

    ring_buffer* rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event, &ctx, nullptr);
    if (!rb) {
        std::cerr << "failed to create ring buffer\n";
        proc_lifecycle__destroy(skel);
        return EXIT_FAILURE;
    }

    std::cerr << "attached; streaming records for cluster=" << opt.cluster << " node=" << opt.node
              << " (Ctrl-C to stop)\n";

    int rc = EXIT_SUCCESS;
    auto last_flush = std::chrono::steady_clock::now();
    auto last_discovery = last_flush;
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
                    ctx.resolver = resolver.get();
                }
            }
        }

        // A partial batch must not sit indefinitely on a quiet node.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_flush).count() >=
            opt.flush_ms) {
            ship(ctx);
            last_flush = now;
        }
    }

    ship(ctx);
    for (slurm_tracer::Sink* s : sinks)
        s->flush();

    std::cerr << "shutting down: " << ctx.events << " events";
    if (resolver) {
        const auto& s = resolver->stats();
        std::cerr << ", attribution hits=" << s.hits << " misses=" << s.misses
                  << " stale=" << s.stale << " rescans=" << s.rescans;
    }
    std::cerr << ", dropped batches=" << stdout_sink.dropped() << "\n";

    ring_buffer__free(rb);
    proc_lifecycle__destroy(skel);
    return rc;
}
