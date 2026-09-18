// Plugin registration.
//
// Probes and sinks are constructed by name, so the daemon never mentions a
// concrete plugin. Each one lives in its own .so, loaded by PluginLoader
// (core/plugin_loader.h) only once a config names it -- nothing is linked
// into the daemon, so a node picks up a new or rebuilt probe by having the
// file on disk and the name in its config, with no new daemon build.
//
// A plugin .so exports exactly one entry point, st_register_probe or
// st_register_sink below, which hands back a factory under the plugin's own
// name. Every plugin uses that same symbol name: they are dlopen'd one at a
// time with RTLD_LOCAL, so nothing collides, and the loader always knows
// which symbol to ask for.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/config/config.h"

namespace slurm_tracer {

class Probe;
class Sink;

template <typename T>
class Registry {
public:
    using Factory = std::function<std::unique_ptr<T>(const ComponentConfig&)>;

    void add(std::string name, Factory factory) {
        factories_.emplace(std::move(name), std::move(factory));
    }

    bool contains(const std::string& name) const { return factories_.count(name) != 0; }

    // Returns nullptr when nothing is registered under `name` — for a freshly
    // loaded plugin that means the .so is not the plugin its filename claims,
    // which the loader reports.
    std::unique_ptr<T> create(const std::string& name, const ComponentConfig& config) const {
        const auto it = factories_.find(name);
        return it == factories_.end() ? nullptr : it->second(config);
    }

    std::vector<std::string> names() const {
        std::vector<std::string> out;
        out.reserve(factories_.size());
        for (const auto& [name, _] : factories_)
            out.push_back(name);
        return out;
    }

private:
    std::map<std::string, Factory> factories_;
};

// The entry point every plugin .so exports, one or the other by kind. Written
// as a plain declaration here so a plugin gets the signature checked against
// what the loader will call, and extern "C" so dlsym has an unmangled name to
// look up.
extern "C" {
void st_register_probe(Registry<Probe>&);
void st_register_sink(Registry<Sink>&);
}

} // namespace slurm_tracer
