// Plugin registries.
//
// Probes and sinks are constructed by name, so the daemon never mentions a
// concrete plugin. Registration happens through registry_manifest.cpp, which
// CMake generates from the set of plugins in the build — see
// cmake/StPlugin.cmake.
//
// Why generated rather than a REGISTER_PROBE static initialiser: the manifest
// calls register_<name>() as a real undefined symbol, so the linker is obliged
// to pull the plugin's translation unit in. A static initialiser inside a static
// library gets dropped silently — no compile error, no link error, the plugin
// simply never appears at runtime.

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/config.h"

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

    // Returns nullptr when nothing is registered under `name` — a config naming
    // a plugin this build does not contain is reported by the caller, which has
    // the context to say so usefully.
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

struct Registries {
    Registry<Probe> probes;
    Registry<Sink> sinks;
};

// Defined by the generated registry_manifest.cpp, not by any hand-written file.
void register_all(Registries&);

} // namespace slurm_tracer
