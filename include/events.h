// Wire format shared between BPF programs and the userspace collector.
//
// Every probe emits records that begin with struct st_event_hdr, so the
// collector can demultiplex a ring buffer without knowing the probe's payload.
// Keep fields naturally aligned and explicitly sized: this struct is memcpy'd
// straight out of the ring buffer.

#ifndef SLURM_TRACER_EVENTS_H
#define SLURM_TRACER_EVENTS_H

// vmlinux.h already provides the __u*/__s* typedefs when compiling BPF.
#ifndef __VMLINUX_H__
#    include <linux/types.h>
#endif

#define ST_COMM_LEN 16

enum st_event_type {
    ST_EVENT_EXEC = 1,
    ST_EVENT_EXIT = 2,
};

struct st_event_hdr {
    __u64 ts_ns;     // bpf_ktime_get_ns(), CLOCK_MONOTONIC
    __u64 cgroup_id; // cgroup v2 id; resolved to a Slurm job/step in userspace
    __u32 type;      // enum st_event_type
    __u32 pid;       // thread-group id (userspace "pid")
    __u32 tid;       // kernel task pid
    __u32 uid;
};

struct st_proc_event {
    struct st_event_hdr hdr;
    __s32 exit_code; // 0 for ST_EVENT_EXEC
    char comm[ST_COMM_LEN];
};

#endif // SLURM_TRACER_EVENTS_H
