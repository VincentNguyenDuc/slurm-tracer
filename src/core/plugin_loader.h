// Loads probe and sink plugins from their own .so files.
//
// Nothing is linked into the daemon: a plugin is found on disk, by a path
// derived from the plugin an instance names, and dlopen'd when that instance
// appears. A reload that drops the instance unloads it again -- so a node can
// pick up a rebuilt probe by replacing the .so and taking the instance out of
// the config and back in, without a new daemon build or a restart.
//
// Handles are closed on unload but deliberately not at teardown: at process
// exit the mappings go away with the process, and closing them would mean
// depending on this object outliving every component it produced.

#pragma once

#include <map>
#include <memory>
#include <string>

#include "core/config/config.h"

namespace slurm_tracer {

class Probe;
class Sink;

class PluginLoader {
public:
    explicit PluginLoader(std::string plugin_dir);

    PluginLoader(const PluginLoader&) = delete;
    PluginLoader& operator=(const PluginLoader&) = delete;

    // Builds the instance `id` from <plugin_dir>/probes/libst_probe_<plugin>.so
    // (sinks/libst_sink_<plugin>.so for a sink). Returns nullptr and logs on
    // any failure -- a plugin that is missing, broken, or refuses its own
    // config disables that one instance, the same as a probe that fails to
    // attach.
    //
    // Two instances of one plugin are two loads of one file: dlopen refcounts
    // them, so they share a mapping and each unload gives back exactly the
    // reference its load took.
    std::unique_ptr<Probe> load_probe(
        const std::string& id, const std::string& plugin, const ComponentConfig& config
    );
    std::unique_ptr<Sink> load_sink(
        const std::string& id, const std::string& plugin, const ComponentConfig& config
    );

    // Drops the reference instance `id` holds on its plugin. The component
    // built for it must already be destroyed: its code, vtable and destructor
    // all live in that mapping.
    void unload_probe(const std::string& id);
    void unload_sink(const std::string& id);

private:
    std::string plugin_dir_;

    // instance id -> dlopen handle, one entry per loaded instance. Separate
    // maps because a probe and a sink may share an id; they are different
    // files.
    std::map<std::string, void*> probe_handles_;
    std::map<std::string, void*> sink_handles_;
};

} // namespace slurm_tracer
