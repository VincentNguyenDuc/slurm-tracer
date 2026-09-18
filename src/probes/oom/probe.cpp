// The oom probe: one record per OOM-killed victim.
//
// This file is the only place the generated skeleton is included, so the
// embedded BPF object does not leak into anything that merely wants the
// translation layer, which is the part worth testing.

#include <spdlog/spdlog.h>

#include <memory>

#include "core/probe/bpf_probe.h"
#include "core/registry.h"
#include "oom.skel.h"
#include "probes/oom/translate.h"

namespace slurm_tracer {
namespace {

class OomProbe : public BpfProbe<oom> {
public:
    OomProbe()
        : BpfProbe<oom>({
              oom__open,
              oom__load,
              oom__attach,
              oom__destroy,
          }) {}

    std::string_view name() const override { return "oom"; }

    int ring_fd() const override {
        return skel() == nullptr ? -1 : bpf_map__fd(skel()->maps.events);
    }

    void on_event(const void* data, size_t len, RecordEmitter& out) override {
        if (len < sizeof(st_oom_event)) {
            spdlog::warn("oom: short event, {} bytes", len);
            return;
        }

        Record r;
        if (translate(*static_cast<const st_oom_event*>(data), r))
            out.emit(std::move(r));
    }
};

} // namespace

// This plugin's one exported symbol, called by PluginLoader after dlopen.
extern "C" void st_register_probe(Registry<Probe>& r) {
    r.add("oom", [](const ComponentConfig&) -> std::unique_ptr<Probe> {
        return std::make_unique<OomProbe>();
    });
}

} // namespace slurm_tracer
