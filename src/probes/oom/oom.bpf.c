// OOM kill probe.
//
// One event per OOM victim selection, attributed to the job whose cgroup the
// victim belonged to -- see docs/DESIGN.md §1 ("what happened in the last 5
// seconds before the OOM?"). Currently invisible to Slurm's own accounting,
// which only samples counters on JobAcctGatherFrequency.

#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "oom_events.h"

char LICENSE[] SEC("license") = "GPL";

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 16); // OOM kills are rare; far smaller than a proc probe's
} events SEC(".maps");

SEC("tp_btf/mark_victim")
int BPF_PROG(on_mark_victim, struct task_struct* victim, uid_t uid) {
    struct st_oom_event* e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
    if (!e)
        return 0;

    e->hdr.type = ST_OOM_EVENT_KILL;
    e->hdr.ts_ns = bpf_ktime_get_ns();
    // bpf_get_current_cgroup_id() only answers for the *current* task, and
    // the OOM killer runs in the allocating task's context, not the
    // victim's -- often, but not always, the same task. Read it straight off
    // the victim instead, same source that helper uses internally.
    e->hdr.cgroup_id = BPF_CORE_READ(victim, cgroups, dfl_cgrp, kn, id);
    e->hdr.pid = BPF_CORE_READ(victim, tgid);
    e->hdr.tid = BPF_CORE_READ(victim, pid);
    e->hdr.uid = uid; // the kernel already resolved this from the victim's cred

    BPF_CORE_READ_STR_INTO(&e->comm, victim, comm);

    bpf_ringbuf_submit(e, 0);
    return 0;
}
