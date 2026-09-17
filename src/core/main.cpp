// slurm-tracer: eBPF observability for Slurm compute nodes.
//
// This file parses arguments and hands over. It names no probe and no sink: the
// daemon builds both from the registry, which the generated manifest populates
// from whatever probes/sinks the build contains.

#include <bpf/libbpf.h>
#include <unistd.h>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "core/config/config.h"
#include "core/config/config_file.h"
#include "core/daemon.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
volatile std::sig_atomic_t g_reload = 0;

void on_stop_signal(int) { g_stop = 1; }
void on_reload_signal(int) { g_reload = 1; }

// libbpf hands us its own level per message; map it onto spdlog's rather than
// dumping raw, unformatted text straight to stderr.
int libbpf_print(enum libbpf_print_level level, const char* fmt, va_list args) {
    if (level == LIBBPF_DEBUG)
        return 0;

    char buf[1024];
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    if (n <= 0)
        return 0;
    // libbpf's format strings already end in '\n'; spdlog adds its own.
    std::string_view msg(buf, static_cast<size_t>(n));
    if (!msg.empty() && msg.back() == '\n')
        msg.remove_suffix(1);

    if (level == LIBBPF_WARN)
        spdlog::warn("libbpf: {}", msg);
    else
        spdlog::info("libbpf: {}", msg);
    return 0;
}

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [options]\n"
              << "  --config <path>       load a JSON config file first; flags below override it\n";
}

bool parse_args(int argc, char** argv, slurm_tracer::Config& config, std::string& config_path) {
    int i = 1;
    while (i < argc) {
        const std::string arg = argv[i];

        if (arg == "--config") {
            if (i + 1 >= argc) {
                spdlog::error("--config: missing argument");
                return false;
            }
            config_path = argv[++i];
            std::string error;
            auto loaded = slurm_tracer::load_config_file(config_path, error);
            if (!loaded) {
                spdlog::error("--config: {}", error);
                return false;
            }
            config = std::move(*loaded);
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            std::exit(EXIT_SUCCESS);
        } else {
            spdlog::error("unknown option: {}", arg);
            usage(argv[0]);
            return false;
        }
        ++i;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    spdlog::set_default_logger(spdlog::stderr_color_mt("slurm-tracer"));
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");

    slurm_tracer::Config config;
    std::string config_path;

    if (!parse_args(argc, argv, config, config_path))
        return EXIT_FAILURE;

    spdlog::set_level(config.verbose ? spdlog::level::debug : spdlog::level::info);

    libbpf_set_print(libbpf_print);
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
    std::signal(SIGHUP, on_reload_signal);

    slurm_tracer::Daemon daemon(std::move(config), std::move(config_path));
    if (!daemon.start())
        return EXIT_FAILURE;
    return daemon.run(g_stop, g_reload);
}
