// slurm-tracer: eBPF observability for Slurm compute nodes.
//
// This file parses arguments and hands over. It names no probe and no sink: the
// daemon builds both from the registry, which the generated manifest populates
// from whatever plugins the build contains. See docs/DESIGN.md.

#include <bpf/libbpf.h>
#include <unistd.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "core/config.h"
#include "runtime/daemon.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

int libbpf_print(enum libbpf_print_level level, const char* fmt, va_list args) {
    if (level == LIBBPF_DEBUG)
        return 0;
    std::vfprintf(stderr, fmt, args);
    return 0;
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [options]\n"
              << "  --cluster <name>      cluster name stamped on every record (default: local)\n"
              << "  --node <name>         node name (default: hostname)\n"
              << "  --cgroup-root <path>  Slurm cgroup root; auto-discovered when omitted\n"
              << "  --probes <a,b,...>    probes to run (default: every one in this build)\n"
              << "  --sinks <a,b,...>     sinks to write to (default: every one in this build)\n"
              << "  --batch-size <n>      records per batch handed to sinks (default: 64)\n"
              << "  --flush-ms <n>        max ms a partial batch waits (default: 1000)\n"
              << "  --verbose             log every resolver decision\n";
}

// "a,b,c" -> one empty ComponentConfig per name. Settings themselves come from
// the config file; the command line only selects.
void parse_names(
    const std::string& list, std::map<std::string, slurm_tracer::ComponentConfig>& out
) {
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        const size_t end = comma == std::string::npos ? list.size() : comma;
        if (end > start)
            out.emplace(list.substr(start, end - start), slurm_tracer::ComponentConfig{});
        if (comma == std::string::npos)
            break;
        start = end + 1;
    }
}

std::string hostname() {
    char buf[256]{};
    if (::gethostname(buf, sizeof(buf) - 1) != 0)
        return "unknown";
    return buf;
}

bool parse_args(int argc, char** argv, slurm_tracer::Config& config) {
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
            config.cluster = v;
        } else if (arg == "--node") {
            const char* v = next("a name");
            if (!v)
                return false;
            config.node = v;
        } else if (arg == "--cgroup-root") {
            const char* v = next("a path");
            if (!v)
                return false;
            config.cgroup_root = v;
        } else if (arg == "--probes") {
            const char* v = next("a comma-separated list");
            if (!v)
                return false;
            parse_names(v, config.probes);
        } else if (arg == "--sinks") {
            const char* v = next("a comma-separated list");
            if (!v)
                return false;
            parse_names(v, config.sinks);
        } else if (arg == "--batch-size") {
            const char* v = next("a count");
            if (!v)
                return false;
            config.batch_size = std::strtoul(v, nullptr, 10);
            if (config.batch_size == 0)
                config.batch_size = 1;
        } else if (arg == "--flush-ms") {
            const char* v = next("a duration");
            if (!v)
                return false;
            config.flush_interval = std::chrono::milliseconds(std::strtoul(v, nullptr, 10));
        } else if (arg == "--verbose") {
            config.verbose = true;
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

} // namespace

int main(int argc, char** argv) {
    slurm_tracer::Config config;
    if (!parse_args(argc, argv, config))
        return EXIT_FAILURE;
    if (config.node.empty())
        config.node = hostname();

    libbpf_set_print(libbpf_print);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    slurm_tracer::Daemon daemon(std::move(config));
    if (!daemon.start())
        return EXIT_FAILURE;
    return daemon.run(g_stop);
}
