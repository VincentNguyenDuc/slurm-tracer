#include "sinks/null/sink.h"

#include <memory>

#include "core/registry.h"

namespace slurm_tracer {

// This plugin's one exported symbol, called by PluginLoader after dlopen.
extern "C" void st_register_sink(Registry<Sink>& r) {
    r.add("null", [](const ComponentConfig&) -> std::unique_ptr<Sink> {
        return std::make_unique<NullSink>();
    });
}

} // namespace slurm_tracer
