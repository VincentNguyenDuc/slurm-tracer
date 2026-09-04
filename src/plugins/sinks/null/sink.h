// Counts records and discards them.
//
// Useful for measuring what the probes and the pipeline cost on their own, with
// the serialisation and I/O of a real sink taken out of the measurement.

#pragma once

#include <atomic>
#include <cstdint>

#include "core/sink.h"

namespace slurm_tracer {

class NullSink : public Sink {
public:
    std::string_view name() const override { return "null"; }

    void write(std::vector<Record> batch) override {
        records_.fetch_add(batch.size(), std::memory_order_relaxed);
    }

    // Nothing is queued, so there is nothing to wait for.
    void flush() override {}

    // Nothing is ever dropped: write() cannot fall behind.
    uint64_t dropped() const override { return 0; }

    uint64_t records() const { return records_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> records_{0};
};

} // namespace slurm_tracer
