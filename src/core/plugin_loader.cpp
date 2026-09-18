#include "core/plugin_loader.h"

#include <dlfcn.h>

#include <spdlog/spdlog.h>

#include <utility>

#include "core/probe/probe.h"
#include "core/registry.h"
#include "core/sink/sink.h"

namespace slurm_tracer {
namespace {

// <dir>/<kind>s/libst_<kind>_<name>.so -- the layout cmake/StPlugin.cmake
// writes into the build tree.
std::string plugin_path(const std::string& dir, const char* kind, const std::string& name) {
    return dir + "/" + kind + "s/libst_" + kind + "_" + name + ".so";
}

// Opens one plugin and builds the single component it registers. On every
// failure path the handle is closed again, so a caller that gets a null
// component has nothing left to clean up.
template <typename T>
std::pair<std::unique_ptr<T>, void*> open_plugin(
    const std::string& path,
    const char* symbol,
    const std::string& id,
    const std::string& plugin,
    const ComponentConfig& config
) {
    // RTLD_NOW so a plugin with an unresolved symbol fails here, while it
    // still costs one disabled component, rather than mid-poll later.
    // RTLD_LOCAL keeps its symbols to itself, which is what lets every
    // plugin export the same entry point name.
    // Instances usually are their plugin, and say so once; one that isn't
    // names both, so a log line about "http_backup" still says it is http.
    const std::string label = id == plugin ? id : id + " (plugin " + plugin + ")";

    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        spdlog::warn("{}: {}", label, dlerror());
        return {nullptr, nullptr};
    }

    auto entry = reinterpret_cast<void (*)(Registry<T>&)>(dlsym(handle, symbol));
    if (entry == nullptr) {
        spdlog::warn("{}: {} exports no {}", label, path, symbol);
        dlclose(handle);
        return {nullptr, nullptr};
    }

    // Looked up by plugin, not by id: the name a plugin registers itself
    // under is its own, and knows nothing about what a config called this
    // instance of it.
    //
    // The registry must also be gone before any dlclose below: the factory it
    // holds is a lambda compiled into the plugin, so destroying the
    // std::function wrapping it runs code from this mapping. Keeping it in
    // its own scope is what makes the failure paths safe to unload from.
    bool registered = false;
    std::unique_ptr<T> component;
    {
        Registry<T> registry;
        entry(registry);
        registered = registry.contains(plugin);
        if (registered)
            component = registry.create(plugin, config);
    }

    if (!registered) {
        spdlog::warn("{}: {} registers something else", label, path);
        dlclose(handle);
        return {nullptr, nullptr};
    }
    // A factory returning null means it rejected its own settings, and said
    // so itself -- no second, vaguer warning on top of that one.
    if (!component) {
        dlclose(handle);
        return {nullptr, nullptr};
    }

    return {std::move(component), handle};
}

void close_handle(std::map<std::string, void*>& handles, const std::string& id) {
    const auto it = handles.find(id);
    if (it == handles.end())
        return;
    dlclose(it->second);
    handles.erase(it);
}

} // namespace

PluginLoader::PluginLoader(std::string plugin_dir)
    : plugin_dir_(std::move(plugin_dir)) {}

std::unique_ptr<Probe> PluginLoader::load_probe(
    const std::string& id, const std::string& plugin, const ComponentConfig& config
) {
    auto [probe, handle] = open_plugin<Probe>(
        plugin_path(plugin_dir_, "probe", plugin), "st_register_probe", id, plugin, config
    );
    if (probe)
        probe_handles_[id] = handle;
    return std::move(probe);
}

std::unique_ptr<Sink> PluginLoader::load_sink(
    const std::string& id, const std::string& plugin, const ComponentConfig& config
) {
    auto [sink, handle] = open_plugin<Sink>(
        plugin_path(plugin_dir_, "sink", plugin), "st_register_sink", id, plugin, config
    );
    if (sink)
        sink_handles_[id] = handle;
    return std::move(sink);
}

void PluginLoader::unload_probe(const std::string& id) { close_handle(probe_handles_, id); }

void PluginLoader::unload_sink(const std::string& id) { close_handle(sink_handles_, id); }

} // namespace slurm_tracer
