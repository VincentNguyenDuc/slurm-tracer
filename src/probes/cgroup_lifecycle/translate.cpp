#include "probes/cgroup_lifecycle/translate.h"

#include <cstring>
#include <iostream>

namespace slurm_tracer {

bool translate(const st_cgroup_event& e, CgroupLifecycleUpdate& out) {
    switch (e.hdr.type) {
    case ST_CGROUP_EVENT_CREATE:
        out.created = true;
        break;
    case ST_CGROUP_EVENT_REMOVE:
        out.created = false;
        break;
    default:
        std::cerr << "cgroup_lifecycle: unknown event type " << e.hdr.type << "\n";
        return false;
    }

    out.cgroup_id = e.hdr.cgroup_id;
    out.ts_ns = e.hdr.ts_ns; // monotonic; matches CgroupResolver's reuse-guard clock domain
    // path is not guaranteed NUL-terminated when it fills the buffer.
    out.path.assign(e.path, ::strnlen(e.path, sizeof(e.path)));
    return true;
}

} // namespace slurm_tracer
