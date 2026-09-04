// The proc_lifecycle probe: one record per exec and per exit.
//
// This file is the only place the generated skeleton is included, so the
// embedded BPF object does not leak into anything that merely wants the
// translation. See translate.h for the part worth testing.

#include <memory>

#include "core/bpf_probe.h"
#include "core/registry.h"
#include "plugins/probes/proc_lifecycle/translate.h"
#include "proc_lifecycle.skel.h"

namespace slurm_tracer {
namespace {

class ProcLifecycleProbe : public BpfProbe<proc_lifecycle> {
public:
    ProcLifecycleProbe()
        : BpfProbe<proc_lifecycle>({
              proc_lifecycle__open,
              proc_lifecycle__load,
              proc_lifecycle__attach,
              proc_lifecycle__destroy,
          }) {}

    std::string_view name() const override { return "proc_lifecycle"; }

    int ring_fd() const override {
        return skel() == nullptr ? -1 : bpf_map__fd(skel()->maps.events);
    }

    void on_event(const void* data, size_t len, RecordEmitter& out) override {
        if (len < sizeof(st_proc_event)) {
            std::cerr << "proc_lifecycle: short event, " << len << " bytes\n";
            return;
        }

        Record r;
        if (translate(*static_cast<const st_proc_event*>(data), r))
            out.emit(std::move(r));
    }
};

} // namespace

// Called by the generated registry_manifest.cpp.
void register_proc_lifecycle(Registries& r) {
    r.probes.add("proc_lifecycle", [](const ComponentConfig&) -> std::unique_ptr<Probe> {
        return std::make_unique<ProcLifecycleProbe>();
    });
}

} // namespace slurm_tracer
