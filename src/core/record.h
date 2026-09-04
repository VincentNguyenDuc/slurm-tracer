// The wide record model from docs/DESIGN.md §9.
//
// One record type for every probe. A new probe adds new `metric` values and new
// `attrs` keys, never new fields — that is what keeps the warehouse schema
// stable as the probe set grows.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace slurm_tracer {

struct Record {
    // Provenance.
    uint64_t ts_ns = 0; // wall clock, nanoseconds since the epoch
    std::string node;
    std::string cluster;
    std::string probe;

    // Attribution. Empty optionals mean "this event could not be tied to a
    // Slurm job" — emitted anyway, because a rising unattributed rate is the
    // signal that the resolver is misconfigured.
    //
    // cgroup_id is what the probe stamps and what the resolver consumes; the
    // rest the pipeline fills in. It is a real field rather than an attrs entry
    // because every probe sets it on every record, and stringifying it into
    // attrs cost an allocation per event.
    uint64_t cgroup_id = 0;
    std::optional<uint32_t> job_id;
    std::optional<std::string> step_id; // "0", "batch", "extern", "interactive"
    std::optional<uint32_t> task_id;
    std::optional<uint32_t> uid;
    std::optional<std::string> user;
    std::optional<std::string> account;   // filled by enrichment, M1: unset
    std::optional<std::string> partition; // filled by enrichment, M1: unset

    // Identity.
    std::string event_type;
    uint32_t pid = 0;
    uint32_t tid = 0;
    std::string comm;

    // Numeric payload.
    std::string metric;
    double value = 0.0;
    std::string unit;

    std::vector<std::pair<std::string, std::string>> attrs;
};

std::string to_json(const Record& r);

} // namespace slurm_tracer
