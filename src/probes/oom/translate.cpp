#include "probes/oom/translate.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace slurm_tracer {

bool translate(const st_oom_event& e, Record& r) {
    switch (e.hdr.type) {
    case ST_OOM_EVENT_KILL:
        r.event_type = "oom_kill";
        r.metric = "oom.kill";
        r.value = 1.0;
        r.unit = "count";
        break;
    default:
        spdlog::warn("oom: unknown event type {}", e.hdr.type);
        return false;
    }

    r.probe = "oom";
    r.ts_ns = e.hdr.ts_ns; // monotonic; the pipeline converts to wall clock
    r.cgroup_id = e.hdr.cgroup_id;
    r.uid = e.hdr.uid;
    r.pid = e.hdr.pid;
    r.tid = e.hdr.tid;
    // comm is not guaranteed NUL-terminated when it fills the buffer.
    r.comm.assign(e.comm, ::strnlen(e.comm, ST_COMM_LEN));
    return true;
}

} // namespace slurm_tracer
