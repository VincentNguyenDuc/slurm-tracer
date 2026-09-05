// Loads a Config from a TOML file, per docs/DESIGN.md §7.
//
// core/config.h is deliberately format-agnostic (see its own header comment);
// this is where that format lives instead. Deliberately not a general TOML
// parser -- only the subset §7 actually uses: `[section]` and
// `[section.name]` headers, `key = value` pairs, bare/quoted/bool values,
// `#` comments. Anything structurally outside that (a stray key with no
// section, an unterminated header, a section this loader does not recognise)
// is a startup failure, not a best-effort skip -- a malformed file is a
// mistake worth catching before the daemon runs one probe.
//
// A key this loader *does* recognise but a component does not is a different
// matter and not this file's business: ComponentConfig hands it to the
// component regardless (core/config.h), which is what lets one config be
// pushed to nodes running different builds.

#pragma once

#include <optional>
#include <string>

#include "core/config.h"

namespace slurm_tracer {

// Returns nullopt and writes a human-readable reason to `error` on any
// failure: the file cannot be opened, or it does not parse.
std::optional<Config> load_config_file(const std::string& path, std::string& error);

} // namespace slurm_tracer
