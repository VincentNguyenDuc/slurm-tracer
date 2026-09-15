// The oom wire format -> Record translation.
//
// Deliberately separate from probe.cpp and free of libbpf, same split
// proc_lifecycle uses: a plain function over a plain struct means a test can
// exercise it with no kernel, no skeleton, and no privileges.

#pragma once

#include "core/record.h"
#include "probes/oom/oom_events.h"

namespace slurm_tracer {

// Fills `r` from `e`. Returns false for an event type this probe does not
// know, in which case `r` is left unspecified and the caller should drop it.
bool translate(const st_oom_event& e, Record& r);

} // namespace slurm_tracer
