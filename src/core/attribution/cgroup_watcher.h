// Feeds CgroupResolver from cgroup_mkdir/rmdir events instead of a filesystem
// poll.
//
// Not a Probe the registry discovers or a config can disable: attribution is
// mandatory, not a pluggable metric, so the daemon builds and attaches this
// directly, right next to the resolver it feeds. It still implements Probe's
// lifecycle (open/load/attach/detach/ring_fd/on_event) purely to reuse
// BpfProbe's skeleton handling and EventLoop's ring-buffer polling.

#pragma once

#include <cstddef>
#include <string_view>

#include "core/attribution/attribution.h"
#include "core/probe/bpf_probe.h"
#include "core/probe/probe.h"

struct cgroup_watcher;

namespace slurm_tracer {

class CgroupWatcher : public BpfProbe<cgroup_watcher> {
public:
    // `resolver` must outlive this watcher; the daemon owns both and
    // constructs them in that order.
    explicit CgroupWatcher(CgroupResolver& resolver);

    std::string_view name() const override { return "cgroup_watcher"; }

    int ring_fd() const override;
    void on_event(const void* data, size_t len, RecordEmitter&) override;

private:
    CgroupResolver& resolver_;
};

} // namespace slurm_tracer
