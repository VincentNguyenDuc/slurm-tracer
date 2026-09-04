// Wire format shared between BPF programs and the userspace collector.
//
// Every probe emits records that begin with struct st_event_hdr, so the
// collector can demultiplex a ring buffer without knowing the probe's payload.
// Keep fields naturally aligned and explicitly sized: this struct is memcpy'd
// straight out of the ring buffer.
//
// Only what every probe shares belongs here. A probe's own event struct lives
// beside its .bpf.c, in that probe's own directory.

#pragma once

// vmlinux.h already provides the __u*/__s* typedefs when compiling BPF.
#ifndef __VMLINUX_H__
#    include <linux/types.h>
#endif

// TASK_COMM_LEN. Shared because any probe that captures a command name uses it.
#define ST_COMM_LEN 16

struct st_event_hdr {
    __u64 ts_ns;     // bpf_ktime_get_ns(), CLOCK_MONOTONIC
    __u64 cgroup_id; // cgroup v2 id; resolved to a Slurm job/step in userspace
    __u32 type;      // probe-defined; see the note below
    __u32 pid;       // thread-group id (userspace "pid")
    __u32 tid;       // kernel task pid
    __u32 uid;
};

// `type` is deliberately not a shared enum. Each probe owns its own ring buffer,
// so the core always knows which probe an event came from and never interprets
// `type` itself — only the owning probe does. A shared enum would mean every new
// probe editing this file, which is exactly what the plugin split exists to avoid.
