// Sink interface, per docs/DESIGN.md §8.
//
// The core fans every batch out to all configured sinks. A sink must never
// block the poll loop: backpressure from a slow destination must not reach the
// BPF ring buffer, because that costs kernel events. Every sink therefore owns
// a bounded queue and a worker thread, drops the oldest batch on overflow, and
// counts the drops so they are visible rather than silent.

#ifndef SLURM_TRACER_SINK_H
#define SLURM_TRACER_SINK_H

#include <cstdint>
#include <string_view>
#include <vector>

#include "record.h"

namespace st {

class Sink {
public:
    virtual ~Sink() = default;
    virtual std::string_view name() const = 0;

    // Hands off a batch. Must return promptly.
    virtual void write(std::vector<Record> batch) = 0;

    // Blocks until queued batches have been written out.
    virtual void flush() = 0;

    virtual uint64_t dropped() const = 0;
};

} // namespace st

#endif // SLURM_TRACER_SINK_H
