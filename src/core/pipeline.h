// The path from a probe's record to the sinks.
//
// Probes emit partly-filled records: whatever only the probe knows (the metric,
// the payload, the cgroup id, the raw monotonic timestamp). Everything a record
// needs but no probe should have to get right — attribution, the uid->user
// lookup, the wall-clock conversion, the node/cluster stamp — happens here,
// once, for every probe.

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/probe.h"
#include "core/record.h"
#include "core/sink.h"

namespace slurm_tracer {

class CgroupResolver;

class Pipeline : public RecordEmitter {
public:
    struct Options {
        std::string node;
        std::string cluster;
        size_t batch_size = 64;
        std::chrono::milliseconds flush_interval{1000};
        bool verbose = false;
    };

    Pipeline(Options opt, std::vector<Sink*> sinks);

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // The resolver can arrive after the pipeline is running: on a compute node
    // the tracer usually starts at boot, before slurmd has created the cgroup
    // scope at all. Records emitted before then go out unattributed.
    void set_resolver(CgroupResolver* resolver) { resolver_ = resolver; }

    // RecordEmitter. Enriches, batches, and ships once the batch is full.
    void emit(Record r) override;

    // Ships a partial batch once flush_interval has elapsed, so a quiet node
    // does not sit on records indefinitely.
    void tick(std::chrono::steady_clock::time_point now);

    // Ships whatever is pending and drains every sink. For shutdown.
    void flush();

    struct Stats {
        uint64_t records = 0;
        uint64_t unattributed = 0;
    };
    const Stats& stats() const { return stats_; }

private:
    void enrich(Record& r);
    void ship();

    // uid -> user name. Cached because getpwuid_r can hit NSS/LDAP, which has no
    // business running once per event. A member rather than a function-local
    // static so a test can construct an isolated pipeline.
    const std::string* lookup_user(uint32_t uid);

    Options opt_;
    std::vector<Sink*> sinks_;
    CgroupResolver* resolver_ = nullptr;

    std::vector<Record> batch_;
    std::chrono::steady_clock::time_point last_flush_;
    std::unordered_map<uint32_t, std::string> user_cache_;
    Stats stats_;
};

} // namespace slurm_tracer
