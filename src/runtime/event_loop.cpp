#include "runtime/event_loop.h"

#include <bpf/libbpf.h>

#include <cerrno>
#include <iostream>

#include "core/probe.h"

namespace slurm_tracer {

EventLoop::~EventLoop() {
    if (rb_ != nullptr)
        ring_buffer__free(rb_);
}

bool EventLoop::add(Probe& probe, RecordEmitter& out) {
    const int fd = probe.ring_fd();
    if (fd < 0)
        return true; // aggregating probe; nothing to poll

    bindings_.push_back(std::unique_ptr<Binding>(new Binding{&probe, &out}));
    void* ctx = bindings_.back().get();

    // Captureless, so it converts to the plain function pointer libbpf wants.
    // The loop does not know what the bytes mean; the probe that owns the
    // buffer does the translating.
    ring_buffer_sample_fn callback = [](void* raw, void* data, size_t size) -> int {
        auto& b = *static_cast<Binding*>(raw);
        b.probe->on_event(data, size, *b.out);
        return 0;
    };

    const bool ok = rb_ == nullptr ? (rb_ = ring_buffer__new(fd, callback, ctx, nullptr)) != nullptr
                                   : ring_buffer__add(rb_, fd, callback, ctx) == 0;
    if (!ok) {
        std::cerr << "probe " << probe.name() << ": failed to arm ring buffer\n";
        bindings_.pop_back();
        return false;
    }
    return true;
}

bool EventLoop::poll(std::chrono::milliseconds timeout) {
    if (rb_ == nullptr)
        return true;

    const int err = ring_buffer__poll(rb_, static_cast<int>(timeout.count()));
    if (err < 0 && err != -EINTR) {
        std::cerr << "ring buffer poll failed: " << err << "\n";
        return false;
    }
    return true;
}

} // namespace slurm_tracer
