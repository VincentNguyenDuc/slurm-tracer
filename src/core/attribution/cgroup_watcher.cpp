// This file is the only place the generated skeleton is included, so the
// embedded BPF object does not leak into anything that merely wants the
// CgroupWatcher type. The translation layer is the part worth testing.

#include "core/attribution/cgroup_watcher.h"

#include <spdlog/spdlog.h>

#include "cgroup_watcher.skel.h"
#include "core/attribution/translate.h"

namespace slurm_tracer {

CgroupWatcher::CgroupWatcher(CgroupResolver& resolver)
    : BpfProbe<cgroup_watcher>({
          cgroup_watcher__open,
          cgroup_watcher__load,
          cgroup_watcher__attach,
          cgroup_watcher__destroy,
      })
    , resolver_(resolver) {}

int CgroupWatcher::ring_fd() const {
    return skel() == nullptr ? -1 : bpf_map__fd(skel()->maps.events);
}

void CgroupWatcher::on_event(const void* data, size_t len, RecordEmitter&) {
    if (len < sizeof(st_cgroup_event)) {
        spdlog::warn("cgroup watcher: short event, {} bytes", len);
        return;
    }

    CgroupLifecycleUpdate update;
    if (!translate(*static_cast<const st_cgroup_event*>(data), update))
        return;

    if (update.created)
        resolver_.on_created(update.cgroup_id, update.path, update.ts_ns);
    else
        resolver_.on_removed(update.cgroup_id, update.ts_ns);
}

} // namespace slurm_tracer
