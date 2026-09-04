#include "core/clock.h"

#include <ctime>

namespace slurm_tracer {
namespace {

uint64_t to_ns(const timespec& ts) {
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

} // namespace

uint64_t monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return to_ns(ts);
}

uint64_t realtime_ns() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return to_ns(ts);
}

uint64_t boot_offset_ns() {
    static const uint64_t offset = realtime_ns() - monotonic_ns();
    return offset;
}

} // namespace slurm_tracer
