// The cgroup_lifecycle probe: feeds CgroupResolver from cgroup_mkdir/rmdir
// events instead of a filesystem poll. See core/attribution.h and
// cgroup_lifecycle.bpf.c.
//
// This file is the only place the generated skeleton is included, so the
// embedded BPF object does not leak into anything that merely wants the
// translation. See translate.h for the part worth testing.

#include <iostream>
#include <memory>

#include "cgroup_lifecycle.skel.h"
#include "core/bpf_probe.h"
#include "core/registry.h"
#include "probes/cgroup_lifecycle/translate.h"

namespace slurm_tracer {
namespace {

class CgroupLifecycleProbe : public BpfProbe<cgroup_lifecycle> {
public:
    CgroupLifecycleProbe()
        : BpfProbe<cgroup_lifecycle>({
              cgroup_lifecycle__open,
              cgroup_lifecycle__load,
              cgroup_lifecycle__attach,
              cgroup_lifecycle__destroy,
          }) {}

    std::string_view name() const override { return "cgroup_lifecycle"; }

    // Losing this probe means every other probe's records go unattributed,
    // not just one metric -- see Daemon::start_probes().
    bool critical() const override { return true; }

    void bind_resolver(CgroupResolver& resolver) override { resolver_ = &resolver; }

    int ring_fd() const override {
        return skel() == nullptr ? -1 : bpf_map__fd(skel()->maps.events);
    }

    void on_event(const void* data, size_t len, RecordEmitter&) override {
        if (len < sizeof(st_cgroup_event)) {
            std::cerr << "cgroup_lifecycle: short event, " << len << " bytes\n";
            return;
        }
        if (resolver_ == nullptr)
            return;

        CgroupLifecycleUpdate update;
        if (!translate(*static_cast<const st_cgroup_event*>(data), update))
            return;

        if (update.created)
            resolver_->on_created(update.cgroup_id, update.path, update.ts_ns);
        else
            resolver_->on_removed(update.cgroup_id, update.ts_ns);
    }

private:
    CgroupResolver* resolver_ = nullptr;
};

} // namespace

// Called by the generated registry_manifest.cpp.
void register_cgroup_lifecycle(Registries& r) {
    r.probes.add("cgroup_lifecycle", [](const ComponentConfig&) -> std::unique_ptr<Probe> {
        return std::make_unique<CgroupLifecycleProbe>();
    });
}

} // namespace slurm_tracer
