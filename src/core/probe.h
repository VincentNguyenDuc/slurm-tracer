// Probe contract, per docs/DESIGN.md §5.
//
// A probe contributes one .bpf.c, one class implementing this interface, and an
// event struct beginning with st_event_hdr. It owns its own ring buffer, so the
// core always knows which probe an event came from and never has to interpret
// the payload.
//
// Probes emit partly-filled records. Attribution, the uid->user lookup, the
// wall-clock conversion, and the node/cluster stamp are the pipeline's job, not
// each probe's — so a new probe gets all of that for free and cannot get it
// subtly wrong.

#pragma once

#include <cstddef>
#include <string_view>

#include "core/config.h"
#include "core/record.h"

namespace slurm_tracer {

// Where a probe hands finished records. Implemented by the pipeline in
// production and by a collecting stub in tests, which is what lets a probe's
// translation be tested with no kernel and no privileges.
class RecordEmitter {
public:
    virtual ~RecordEmitter() = default;
    virtual void emit(Record r) = 0;
};

class Probe {
public:
    virtual ~Probe() = default;

    virtual std::string_view name() const = 0;

    // Lifecycle. Split because the failure modes are worth telling apart: load()
    // is where the verifier runs, attach() is where a missing tracepoint shows
    // up. Any of them returning false disables this probe alone — a node with an
    // older kernel loses one probe, not all observability.
    virtual bool open(const ComponentConfig&) = 0;
    virtual bool load() = 0;
    virtual bool attach() = 0;
    virtual void detach() = 0;

    // Every probe exposes a ring buffer for the event loop to poll and
    // translate each event in on_event(). See DESIGN §6: this tree only
    // supports the event-driven shape, not in-kernel aggregation.
    virtual int ring_fd() const { return -1; }
    virtual void on_event(const void* data, size_t len, RecordEmitter&) = 0;
};

} // namespace slurm_tracer
