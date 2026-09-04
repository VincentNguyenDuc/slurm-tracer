#include "plugins/sinks/null/sink.h"

#include <memory>

#include "core/registry.h"

namespace slurm_tracer {

// Called by the generated registry_manifest.cpp.
void register_null(Registries& r) {
    r.sinks.add("null", [](const ComponentConfig&) -> std::unique_ptr<Sink> {
        return std::make_unique<NullSink>();
    });
}

} // namespace slurm_tracer
