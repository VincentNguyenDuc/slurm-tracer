#include "event_loop.h"

#include <bpf/libbpf.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cerrno>

#include "core/probe/probe.h"

namespace slurm_tracer {

EventLoop::~EventLoop() {
    if (rb_ != nullptr)
        ring_buffer__free(rb_);
}

bool EventLoop::add(Probe& probe, RecordEmitter& out) {
    const int fd = probe.ring_fd();
    if (fd < 0)
        return true; // no ring buffer to poll

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
        spdlog::warn("probe {}: failed to arm ring buffer", probe.name());
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
        spdlog::error("ring buffer poll failed: {}", err);
        return false;
    }
    return true;
}

std::vector<Probe*> EventLoop::remove(Probe& probe) {
    std::vector<Probe*> dropped;

    const auto it =
        std::find_if(bindings_.begin(), bindings_.end(), [&](const std::unique_ptr<Binding>& b) {
            return b->probe == &probe;
        });
    if (it == bindings_.end())
        return dropped;
    bindings_.erase(it); // Binding is heap-allocated; erase doesn't move it or
    // any other Binding's address, which is what libbpf's
    // ctx pointers below are keyed on.

    if (rb_ != nullptr) {
        ring_buffer__free(rb_);
        rb_ = nullptr;
    }
    if (bindings_.empty())
        return dropped; // matches the existing empty() contract

    ring_buffer_sample_fn callback = [](void* raw, void* data, size_t size) -> int {
        auto& b = *static_cast<Binding*>(raw);
        b.probe->on_event(data, size, *b.out);
        return 0;
    };

    // A binding that fails to re-arm here is not just left broken: it would
    // stay listed as running in bindings_/the caller's own probe list while
    // never producing another event, a zombie indistinguishable from a
    // healthy probe. It is dropped from bindings_ (its Probe* reported back
    // instead, via `dropped`) so the caller can detach and forget it too --
    // the same fate as a probe that fails to attach at startup.
    std::vector<std::unique_ptr<Binding>> surviving;
    surviving.reserve(bindings_.size());
    for (auto& binding : bindings_) {
        const int fd = binding->probe->ring_fd(); // re-derived fresh each call
        const bool ok =
            rb_ == nullptr
                ? (rb_ = ring_buffer__new(fd, callback, binding.get(), nullptr)) != nullptr
                : ring_buffer__add(rb_, fd, callback, binding.get()) == 0;
        if (ok) {
            surviving.push_back(std::move(binding));
        } else {
            spdlog::error(
                "probe {}: failed to re-arm while removing {}; dropping it too",
                binding->probe->name(),
                probe.name()
            );
            dropped.push_back(binding->probe);
        }
    }
    bindings_ = std::move(surviving);

    return dropped;
}

} // namespace slurm_tracer
