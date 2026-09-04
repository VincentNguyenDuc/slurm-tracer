// The sched_latency probe: reads the kernel-side latency histogram on each
// poll() and turns it into one Record per (cgroup, non-empty bucket).
//
// This file is the only place the generated skeleton is included, same
// convention as proc_lifecycle/probe.cpp.

#include <bpf/bpf.h>

#include <memory>
#include <string>
#include <vector>

#include "core/bpf_probe.h"
#include "core/clock.h"
#include "core/registry.h"
#include "sched_latency.skel.h"
#include "sched_latency_events.h"

namespace slurm_tracer {
namespace {

class SchedLatencyProbe : public BpfProbe<sched_latency> {
public:
    SchedLatencyProbe()
        : BpfProbe<sched_latency>({
              sched_latency__open,
              sched_latency__load,
              sched_latency__attach,
              sched_latency__destroy,
          }) {}

    std::string_view name() const override { return "sched_latency"; }

    // Aggregating probe (DESIGN §6): nothing arrives via a ring buffer, so
    // there is no ring_fd() override (the base class default of -1 is
    // correct) and nothing to demux here.
    void on_event(const void*, size_t, RecordEmitter&) override {}

    void poll(RecordEmitter& out) override {
        if (skel() == nullptr)
            return;
        const int map_fd = bpf_map__fd(skel()->maps.hist);
        const uint64_t now_ns = monotonic_ns();

        // Collect keys before mutating the map: deleting entries while
        // bpf_map_get_next_key() is mid-iteration is asking for skipped or
        // repeated keys, so lookup+delete happens in a second pass instead.
        std::vector<uint64_t> keys;
        uint64_t key = 0;
        uint64_t next_key = 0;
        for (int err = bpf_map_get_next_key(map_fd, nullptr, &next_key); err == 0;
             err = bpf_map_get_next_key(map_fd, &key, &next_key)) {
            keys.push_back(next_key);
            key = next_key;
        }

        for (const uint64_t cgroup_id : keys) {
            st_sched_latency_hist hist{};
            if (bpf_map_lookup_elem(map_fd, &cgroup_id, &hist) == 0)
                emit_histogram(cgroup_id, hist, now_ns, out);
            // Records carry per-interval counts, not a running total: reset
            // so a cgroup that goes quiet stops being reported at all rather
            // than repeating its last count forever.
            bpf_map_delete_elem(map_fd, &cgroup_id);
        }
    }

private:
    static void emit_histogram(
        uint64_t cgroup_id, const st_sched_latency_hist& hist, uint64_t now_ns, RecordEmitter& out
    ) {
        for (int slot = 0; slot < ST_SCHED_LATENCY_SLOTS; ++slot) {
            if (hist.slots[slot] == 0)
                continue;

            Record r;
            r.probe = "sched_latency";
            r.ts_ns = now_ns; // this is a flush-time aggregate, not a single event's timestamp
            r.cgroup_id = cgroup_id;
            r.event_type = "runq_latency";
            r.metric = "sched.runq_latency";
            r.value = static_cast<double>(hist.slots[slot]);
            r.unit = "count";

            const uint64_t lo_us = slot == 0 ? 0 : (1ull << slot);
            const uint64_t hi_us = 1ull << (slot + 1);
            r.attrs.emplace_back("bucket_us_lo", std::to_string(lo_us));
            r.attrs.emplace_back("bucket_us_hi", std::to_string(hi_us));

            out.emit(std::move(r));
        }
    }
};

} // namespace

// Called by the generated registry_manifest.cpp.
void register_sched_latency(Registries& r) {
    r.probes.add("sched_latency", [](const ComponentConfig&) -> std::unique_ptr<Probe> {
        return std::make_unique<SchedLatencyProbe>();
    });
}

} // namespace slurm_tracer
