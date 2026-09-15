// Cgroup lifecycle probe.
//
// Attribution's whole job is mapping a kernel-side cgroup id to a Slurm job
// (docs/DESIGN.md §4); this probe is what makes that mapping event-driven
// instead of polling the filesystem and hoping the directory is still there.
// cgroup:cgroup_mkdir and cgroup:cgroup_rmdir fire synchronously, in-kernel,
// as part of the mkdir/rmdir syscall itself -- by the time this event reaches
// userspace and CgroupResolver::on_created() runs, no other probe's event for
// that cgroup id could have arrived first.

#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "cgroup_lifecycle_events.h"

char LICENSE[] SEC("license") = "GPL";

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 16); // cgroup churn is rare; far smaller than a proc probe's
} events SEC(".maps");

static __always_inline int emit(
    enum st_cgroup_event_type type, struct cgroup* cgrp, const char* path
) {
    struct st_cgroup_event* e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->hdr.type = type;
    e->hdr.ts_ns = bpf_ktime_get_ns();
    // The cgroup id *is* the kernfs node's id -- the same value
    // bpf_get_current_cgroup_id() (every other probe) and userspace stat()
    // report for this directory. See core/attribution.cpp.
    e->hdr.cgroup_id = BPF_CORE_READ(cgrp, kn, id);
    e->hdr.pid = 0;
    e->hdr.tid = 0;
    e->hdr.uid = 0;

    if (path)
        bpf_probe_read_kernel_str(e->path, sizeof(e->path), path);
    else
        e->path[0] = '\0';

    bpf_ringbuf_submit(e, 0);
    return 0;
}

SEC("tp_btf/cgroup_mkdir")
int BPF_PROG(on_mkdir, struct cgroup* cgrp, const char* path) {
    return emit(ST_CGROUP_EVENT_CREATE, cgrp, path);
}

SEC("tp_btf/cgroup_rmdir")
int BPF_PROG(on_rmdir, struct cgroup* cgrp, const char* path) {
    // path isn't needed to resolve a removal (userspace looks up by id), but
    // capturing it costs nothing and keeps both events symmetric for logging.
    return emit(ST_CGROUP_EVENT_REMOVE, cgrp, path);
}
