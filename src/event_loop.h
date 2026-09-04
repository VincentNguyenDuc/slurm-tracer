// Ring buffer polling for event-driven probes.
//
// One ring buffer per probe, per docs/DESIGN.md §5: it costs a few MiB, and in
// exchange a chatty probe cannot starve a quiet one and each buffer is sized to
// its own event rate. libbpf multiplexes them onto a single epoll set, so this
// is still one poll call.

#pragma once

#include <chrono>
#include <memory>
#include <vector>

struct ring_buffer;

namespace slurm_tracer {

class Probe;
class RecordEmitter;

class EventLoop {
public:
    EventLoop() = default;
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // Adds a probe's ring buffer. Probes that aggregate in-kernel report a
    // ring_fd() of -1 and are simply not added — that is not an error.
    // Returns false only when a probe offered a buffer that could not be armed.
    bool add(Probe& probe, RecordEmitter& out);

    bool empty() const { return rb_ == nullptr; }

    // Waits up to `timeout` for events, dispatching each to its owning probe.
    // Returns false on a fatal poll error; EINTR is not one.
    bool poll(std::chrono::milliseconds timeout);

private:
    // One per probe, heap-allocated because libbpf keeps the pointer.
    struct Binding {
        Probe* probe;
        RecordEmitter* out;
    };

    ring_buffer* rb_ = nullptr;
    std::vector<std::unique_ptr<Binding>> bindings_;
};

} // namespace slurm_tracer
