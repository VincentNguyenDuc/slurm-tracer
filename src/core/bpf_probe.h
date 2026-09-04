// Skeleton lifecycle for BPF-backed probes.
//
// Every libbpf skeleton exposes the same four entry points under
// <name>__open / __load / __attach / __destroy. Wrapping that convention here
// means a probe declares its lifecycle instead of repeating the same four
// error-handling blocks, and the messages stay consistent across probes.
//
// This is the layer that touches libbpf. Nothing in core/ may include it.

#pragma once

#include <bpf/libbpf.h>

#include <iostream>

#include "core/probe.h"

namespace slurm_tracer {

template <typename Skel>
class BpfProbe : public Probe {
public:
    // The generated skeleton's four functions, passed in by the probe because
    // their names are derived from the probe's own name.
    struct Ops {
        Skel* (*open)();
        int (*load)(Skel*);
        int (*attach)(Skel*);
        void (*destroy)(Skel*);
    };

    explicit BpfProbe(Ops ops)
        : ops_(ops) {}

    ~BpfProbe() override { BpfProbe::detach(); }

    bool open(const ComponentConfig&) override {
        skel_ = ops_.open();
        if (skel_ == nullptr) {
            fail("open");
            return false;
        }
        return true;
    }

    // Where the verifier runs, and so the most likely place for a probe to be
    // rejected on an older kernel.
    bool load() override {
        if (skel_ == nullptr || ops_.load(skel_) != 0) {
            fail("load (needs CAP_BPF and CAP_PERFMON)");
            return false;
        }
        return true;
    }

    // Where a missing tracepoint shows up.
    bool attach() override {
        if (skel_ == nullptr || ops_.attach(skel_) != 0) {
            fail("attach");
            return false;
        }
        return true;
    }

    void detach() override {
        if (skel_ != nullptr) {
            ops_.destroy(skel_);
            skel_ = nullptr;
        }
    }

protected:
    Skel* skel() const { return skel_; }

private:
    void fail(const char* stage) const {
        std::cerr << "probe " << name() << ": failed to " << stage << "\n";
    }

    Ops ops_;
    Skel* skel_ = nullptr;
};

} // namespace slurm_tracer
