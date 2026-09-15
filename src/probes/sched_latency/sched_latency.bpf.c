// Run-queue latency probe: how long a task waits runnable before it actually
// runs, bucketed per cgroup. Answers docs/DESIGN.md §1's "why was the job
// slow" when the cause is scheduling contention rather than the job itself.
//
// An aggregate, not an event stream (§6): wakeups/switches on a busy node
// routinely exceed the ~10^4/s streaming budget, so the histogram is built
// here and userspace only ever reads the finished buckets on each poll().

#include "vmlinux.h"

#include <bpf/bpf_core_read.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#include "sched_latency_events.h"

char LICENSE[] SEC("license") = "GPL";

// pid -> the CLOCK_MONOTONIC timestamp it last became runnable. LRU so a task
// that wakes and is never switched in (migrated, re-woken, killed) cannot
// leak an entry forever.
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, 10240);
    __type(key, __u32);
    __type(value, __u64);
} start SEC(".maps");

// cgroup id -> histogram. Sized for a heavily job-packed node; entries are
// deleted by userspace once read, so this only needs to hold whatever is
// currently active, not a running total.
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u64);
    __type(value, struct st_sched_latency_hist);
} hist SEC(".maps");

// bpf_get_current_cgroup_id() only ever answers for `current`, and at both
// sched_wakeup and sched_switch the task we care about (the one waking, or
// the one about to run) is a parameter, not `current` — the tracepoint fires
// before the actual context switch. Reading the cgroup straight off the
// task_struct is the standard way around that; see the note in
// src/core/attribution.cpp on why attribution otherwise stays in userspace.
static __always_inline __u64 task_cgroup_id(struct task_struct* task) {
    struct css_set* cgroups = BPF_CORE_READ(task, cgroups);
    struct cgroup* cgrp = BPF_CORE_READ(cgroups, dfl_cgrp);
    struct kernfs_node* kn = BPF_CORE_READ(cgrp, kn);
    return BPF_CORE_READ(kn, id);
}

static __always_inline __u32 bucket_of(__u64 delta_ns) {
    const __u64 us = delta_ns / 1000;
    __u32 slot = 0;
#pragma unroll
    for (slot = 0; slot < ST_SCHED_LATENCY_SLOTS - 1; slot++) {
        if (us < (1ull << (slot + 1)))
            break;
    }
    return slot;
}

static __always_inline int record_wakeup(struct task_struct* p) {
    __u32 pid = BPF_CORE_READ(p, pid);
    __u64 ts = bpf_ktime_get_ns();
    bpf_map_update_elem(&start, &pid, &ts, BPF_ANY);
    return 0;
}

SEC("tp_btf/sched_wakeup")
int BPF_PROG(on_wakeup, struct task_struct* p) { return record_wakeup(p); }

SEC("tp_btf/sched_wakeup_new")
int BPF_PROG(on_wakeup_new, struct task_struct* p) { return record_wakeup(p); }

SEC("tp_btf/sched_switch")
int BPF_PROG(on_switch, bool preempt, struct task_struct* prev, struct task_struct* next) {
    __u32 next_pid = BPF_CORE_READ(next, pid);
    __u64* start_ts = bpf_map_lookup_elem(&start, &next_pid);
    if (!start_ts)
        return 0; // never saw this task wake up (e.g. running since before we attached)

    const __u64 delta = bpf_ktime_get_ns() - *start_ts;
    bpf_map_delete_elem(&start, &next_pid);

    const __u64 cgid = task_cgroup_id(next);
    const __u32 slot = bucket_of(delta);

    struct st_sched_latency_hist* h = bpf_map_lookup_elem(&hist, &cgid);
    if (!h) {
        struct st_sched_latency_hist zero = {};
        // BPF_NOEXIST: if another CPU raced us to create this cgroup's entry,
        // its zeroed map is just as good as ours -- either way the next
        // lookup finds a live histogram.
        bpf_map_update_elem(&hist, &cgid, &zero, BPF_NOEXIST);
        h = bpf_map_lookup_elem(&hist, &cgid);
        if (!h)
            return 0;
    }
    __sync_fetch_and_add(&h->slots[slot], 1);
    return 0;
}
