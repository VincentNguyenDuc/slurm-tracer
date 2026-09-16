// Wire format private to the cgroup watcher.
//
// Shared between cgroup_watcher.bpf.c and the userspace side of the watcher,
// and by nothing else. The core demuxes on st_event_hdr alone (see
// core/events.h) and never looks past it.

#ifndef SLURM_TRACER_CGROUP_WATCHER_EVENTS_H
#define SLURM_TRACER_CGROUP_WATCHER_EVENTS_H

#include "core/events.h"

// Slurm cgroup paths (.../job_<id>/step_<id>/task_<id>) are short; this is
// generous headroom, not a tight fit.
#define ST_CGROUP_PATH_LEN 256

enum st_cgroup_event_type {
    ST_CGROUP_EVENT_CREATE = 1,
    ST_CGROUP_EVENT_REMOVE = 2,
};

struct st_cgroup_event {
    struct st_event_hdr hdr;
    char path[ST_CGROUP_PATH_LEN]; // absolute cgroupfs path
};

#endif // SLURM_TRACER_CGROUP_WATCHER_EVENTS_H
