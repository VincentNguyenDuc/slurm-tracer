#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include "core/sink.h"

namespace slurm_tracer {

// Writes one JSON object per record to a stream (stdout by default), NDJSON.
// Trivially debuggable and pipes into any existing log shipper.
class StdoutJsonSink : public Sink {
public:
    explicit StdoutJsonSink(size_t max_queued_batches = 1024, std::FILE* out = stdout);
    ~StdoutJsonSink() override;

    std::string_view name() const override { return "stdout_json"; }
    void write(std::vector<Record> batch) override;
    void flush() override;
    uint64_t dropped() const override { return dropped_.load(std::memory_order_relaxed); }

private:
    void run();

    const size_t max_queued_;
    std::FILE* out_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<Record>> queue_;
    bool stopping_ = false;
    uint64_t written_batches_ = 0; // batches fully written, for flush()
    uint64_t queued_batches_ = 0;  // batches ever accepted
    std::condition_variable drained_cv_;

    std::atomic<uint64_t> dropped_{0};
    std::thread worker_;
};

} // namespace slurm_tracer
