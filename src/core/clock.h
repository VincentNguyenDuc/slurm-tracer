// Clocks.
//
// BPF stamps CLOCK_MONOTONIC (bpf_ktime_get_ns), but records carry wall clock so
// they join against Slurm's accounting database. Both clocks and the offset
// between them live here so there is exactly one definition of each.

#pragma once

#include <cstdint>

namespace slurm_tracer {

// CLOCK_MONOTONIC nanoseconds — the same clock as bpf_ktime_get_ns().
uint64_t monotonic_ns();

// CLOCK_REALTIME nanoseconds since the epoch.
uint64_t realtime_ns();

// CLOCK_REALTIME - CLOCK_MONOTONIC, sampled once on first use. Add it to a BPF
// timestamp to get wall clock; add it to a monotonic timestamp to compare
// against a file's ctime.
//
// Sampled once on purpose: re-sampling per event would let NTP steps show up as
// non-monotonic record timestamps within a single job.
uint64_t boot_offset_ns();

} // namespace slurm_tracer
