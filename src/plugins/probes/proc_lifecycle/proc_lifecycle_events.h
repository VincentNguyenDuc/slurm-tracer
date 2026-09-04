// Wire format private to the proc_lifecycle probe.
//
// Shared between proc_lifecycle.bpf.c and the userspace side of this probe, and
// by nothing else. The core demuxes on st_event_hdr alone (see core/events.h)
// and never looks past it.

#ifndef SLURM_TRACER_PROC_LIFECYCLE_EVENTS_H
#define SLURM_TRACER_PROC_LIFECYCLE_EVENTS_H

#include "core/events.h"

enum st_proc_event_type {
    ST_EVENT_EXEC = 1,
    ST_EVENT_EXIT = 2,
};

struct st_proc_event {
    struct st_event_hdr hdr;
    __s32 exit_code; // 0 for ST_EVENT_EXEC
    char comm[ST_COMM_LEN];
};

#endif // SLURM_TRACER_PROC_LIFECYCLE_EVENTS_H
