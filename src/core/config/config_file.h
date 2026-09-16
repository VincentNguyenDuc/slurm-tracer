// Loads a Config from a JSON file.
//
// Config is deliberately format-agnostic (see its own header comment); this
// is where that format lives instead. The schema is a direct translation of
// that shape: a "node" object, a "slurm" object, and "probes"/"sinks" objects
// keyed by component name, each holding that component's own flat key/value
// settings. Parsed with nlohmann::json's ignore_comments option, so `//` and
// `/* */` comments stay legal despite plain JSON having no comment syntax.
// Anything structurally outside that shape (a key this loader does not
// recognise inside "node"/"slurm", a top-level key that isn't one of the four
// sections, the wrong JSON type where an object was expected) is a startup
// failure, not a best-effort skip -- a malformed file is a mistake worth
// catching before the daemon runs one probe.
//
// A key this loader *does* recognise but a component does not is a different
// matter and not this file's business: ComponentConfig hands it to the
// component regardless, which is what lets one config be pushed to nodes
// running different builds.

#pragma once

#include <optional>
#include <string>

#include "core/config/config.h"

namespace slurm_tracer {

// Returns nullopt and writes a human-readable reason to `error` on any
// failure: the file cannot be opened, or it does not parse.
std::optional<Config> load_config_file(const std::string& path, std::string& error);

} // namespace slurm_tracer
