// Process lifecycle probe.
//
// Emits one event per exec and per exit, tagged with the cgroup id of the
// running task. Userspace resolves that cgroup id to a Slurm job/step; see
// docs/DESIGN.md for the attribution model.

#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "events.h"

char LICENSE[] SEC("license") = "GPL";

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 20); // 1 MiB
} events SEC(".maps");

// Set from userspace before load: only report tasks under this cgroup subtree
// root, or 0 to report everything. Kept as a knob so a node daemon can ignore
// non-Slurm activity without paying the ring buffer cost.
const volatile __u64 filter_cgroup_id = 0;

static __always_inline int emit(enum st_event_type type, __s32 exit_code) {
    __u64 cgroup_id = bpf_get_current_cgroup_id();
    if (filter_cgroup_id && cgroup_id != filter_cgroup_id)
        return 0;

    struct st_proc_event* e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;

    __u64 pid_tgid = bpf_get_current_pid_tgid();

    e->hdr.type = type;
    e->hdr.ts_ns = bpf_ktime_get_ns();
    e->hdr.cgroup_id = cgroup_id;
    e->hdr.pid = (__u32)(pid_tgid >> 32);
    e->hdr.tid = (__u32)pid_tgid;
    e->hdr.uid = (__u32)bpf_get_current_uid_gid();
    e->exit_code = exit_code;
    bpf_get_current_comm(&e->comm, sizeof(e->comm));

    bpf_ringbuf_submit(e, 0);
    return 0;
}

SEC("tp_btf/sched_process_exec")
int BPF_PROG(on_exec, struct task_struct* p, pid_t old_pid, struct linux_binprm* bprm) {
    return emit(ST_EVENT_EXEC, 0);
}

SEC("tp_btf/sched_process_exit")
int BPF_PROG(on_exit, struct task_struct* p) {
    // Only report thread-group leaders; per-thread exits are noise for job accounting.
    if (BPF_CORE_READ(p, pid) != BPF_CORE_READ(p, tgid))
        return 0;
    return emit(ST_EVENT_EXIT, BPF_CORE_READ(p, exit_code));
}
