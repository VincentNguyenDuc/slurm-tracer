// The proc_lifecycle wire format -> Record translation.
//
// Deliberately separate from probe.cpp and free of libbpf: everything
// interesting about this probe (the hdr.type dispatch, how the kernel packs an
// exit code, comm truncation) is decided here, and keeping it in a plain
// function over a plain struct means a test can exercise all of it without a
// kernel, a skeleton, or any privileges.

#pragma once

#include "core/record.h"
#include "plugins/probes/proc_lifecycle/proc_lifecycle_events.h"

namespace slurm_tracer {

// Fills `r` from `e`. Returns false for an event type this probe does not know,
// in which case `r` is left unspecified and the caller should drop it.
//
// Only the fields a probe can know are set. The wall-clock conversion, the
// job/step attribution, the uid->user lookup and the node/cluster stamp all
// belong to the pipeline; r.ts_ns is left on the kernel's monotonic clock.
bool translate(const st_proc_event& e, Record& r);

} // namespace slurm_tracer
