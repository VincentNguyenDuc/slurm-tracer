// Wire format private to the oom probe.
//
// Shared between the kernel and userspace sides of this probe, and by nothing
// else. The core demuxes on st_event_hdr alone and never looks past it.

#ifndef SLURM_TRACER_OOM_EVENTS_H
#define SLURM_TRACER_OOM_EVENTS_H

#include "core/events.h"

enum st_oom_event_type {
    ST_OOM_EVENT_KILL = 1,
};

struct st_oom_event {
    struct st_event_hdr hdr;
    char comm[ST_COMM_LEN]; // the victim's, not the allocating task's
};

#endif // SLURM_TRACER_OOM_EVENTS_H
