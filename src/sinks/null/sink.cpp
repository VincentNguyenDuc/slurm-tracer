#include "sinks/null/sink.h"

#include <memory>

#include "core/registry.h"

namespace slurm_tracer {

// Called by the generated plugin manifest.
void register_null(Registries& r) {
    r.sinks.add("null", [](const ComponentConfig&) -> std::unique_ptr<Sink> {
        return std::make_unique<NullSink>();
    });
}

} // namespace slurm_tracer
