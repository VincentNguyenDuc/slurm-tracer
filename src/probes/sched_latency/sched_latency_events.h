// Wire format private to the sched_latency probe.
//
// Unlike proc_lifecycle this probe never touches a ring buffer: it is an
// aggregating probe per docs/DESIGN.md §6 (run-queue latency can exceed
// 10^4 events/s on a busy node, so it is bucketed in-kernel instead of
// streamed). This struct is the BPF hash map's value type, read directly out
// of the map by userspace on each poll() rather than demuxed from events.

#ifndef SLURM_TRACER_SCHED_LATENCY_EVENTS_H
#define SLURM_TRACER_SCHED_LATENCY_EVENTS_H

// vmlinux.h already provides __u32 when compiling BPF.
#ifndef __VMLINUX_H__
#    include <linux/types.h>
#endif

// Log2(microseconds) buckets: slot i covers [2^i, 2^(i+1)) us, except slot 0
// which covers [0, 2) us. 27 slots reaches ~67 seconds, comfortably past any
// run-queue wait worth distinguishing from "the node is broken."
#define ST_SCHED_LATENCY_SLOTS 27

struct st_sched_latency_hist {
    __u32 slots[ST_SCHED_LATENCY_SLOTS];
};

#endif // SLURM_TRACER_SCHED_LATENCY_EVENTS_H
