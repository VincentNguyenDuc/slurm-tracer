// The cgroup_lifecycle wire format -> CgroupLifecycleUpdate translation.
//
// Deliberately separate from probe.cpp and free of libbpf, same split
// proc_lifecycle uses: a plain function over a plain struct means a test can
// exercise it with no kernel, no skeleton, and no privileges.

#pragma once

#include <cstdint>
#include <string>

#include "probes/cgroup_lifecycle/cgroup_lifecycle_events.h"

namespace slurm_tracer {

// A parsed cgroup_mkdir/cgroup_rmdir event. Fed straight into CgroupResolver
// (on_created()/on_removed()) rather than emitted as a Record -- this probe
// *is* the attribution mechanism, not a telemetry source.
struct CgroupLifecycleUpdate {
    bool created = false; // false means removed
    uint64_t cgroup_id = 0;
    uint64_t ts_ns = 0; // CLOCK_MONOTONIC (bpf_ktime_get_ns())
    std::string path;   // absolute cgroupfs path; only meaningful when created
};

// Fills `out` from `e`. Returns false for an event type this probe does not
// know, in which case `out` is left unspecified and the caller should drop it.
bool translate(const st_cgroup_event& e, CgroupLifecycleUpdate& out);

} // namespace slurm_tracer
