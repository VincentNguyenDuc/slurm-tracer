#include "plugins/probes/proc_lifecycle/translate.h"

#include <cstring>
#include <iostream>
#include <string>

namespace slurm_tracer {

bool translate(const st_proc_event& e, Record& r) {
    switch (e.hdr.type) {
    case ST_EVENT_EXEC:
        r.event_type = "exec";
        r.metric = "proc.exec";
        r.value = 1.0;
        r.unit = "count";
        break;
    case ST_EVENT_EXIT:
        r.event_type = "exit";
        r.metric = "proc.exit";
        // The kernel's exit_code packs the wait(2) status: high byte is the
        // exit status, low 7 bits the terminating signal.
        r.value = static_cast<double>((e.exit_code >> 8) & 0xff);
        r.unit = "exit_status";
        if (const int sig = e.exit_code & 0x7f; sig != 0)
            r.attrs.emplace_back("signal", std::to_string(sig));
        break;
    default:
        std::cerr << "proc_lifecycle: unknown event type " << e.hdr.type << "\n";
        return false;
    }

    r.probe = "proc_lifecycle";
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
