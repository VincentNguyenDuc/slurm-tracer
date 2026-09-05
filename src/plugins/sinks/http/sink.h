// Streams NDJSON batches to an HTTP endpoint over a kept-alive connection.
//
// Hand-rolled HTTP/1.1 client over a raw POSIX socket rather than a library:
// libbpf is the project's only external dependency (root CMakeLists.txt), and
// this only ever needs to do one thing (POST a body, read enough of the
// response to reuse the connection) plainly enough that a real client would
// be more surface than the job needs. Plain http:// only -- no TLS here.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/sink.h"

namespace slurm_tracer {

class HttpSink : public Sink {
public:
    struct Options {
        std::string host;
        uint16_t port = 80;
        std::string path = "/";
        size_t max_queued_batches = 1024;
        std::chrono::milliseconds timeout{5000};
    };

    explicit HttpSink(Options opt);
    ~HttpSink() override;

    HttpSink(const HttpSink&) = delete;
    HttpSink& operator=(const HttpSink&) = delete;

    std::string_view name() const override { return "http"; }
    void write(std::vector<Record> batch) override;
    void flush() override;
    uint64_t dropped() const override { return dropped_.load(std::memory_order_relaxed); }

private:
    void run();

    // Sends one NDJSON body and drains just enough of the response to know
    // the connection is still good to reuse. False on anything that costs the
    // batch: connect failure, a write that doesn't complete, a response this
    // client can't frame.
    bool post(const std::string& body);
    bool ensure_connected();
    void close_connection();

    const Options opt_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::vector<Record>> queue_;
    bool stopping_ = false;
    uint64_t written_batches_ = 0; // batches fully handled (sent or dropped), for flush()
    uint64_t queued_batches_ = 0;  // batches ever accepted
    std::condition_variable drained_cv_;

    std::atomic<uint64_t> dropped_{0};
    std::thread worker_;

    // Touched only by the worker thread (run()/post()/ensure_connected()), so
    // it needs no lock of its own.
    int fd_ = -1;
};

} // namespace slurm_tracer
