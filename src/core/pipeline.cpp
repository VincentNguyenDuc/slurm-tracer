#include "core/pipeline.h"

#include <pwd.h>

#include <iostream>
#include <utility>

#include "core/attribution.h"
#include "core/clock.h"

namespace slurm_tracer {

Pipeline::Pipeline(Options opt, std::vector<Sink*> sinks)
    : opt_(std::move(opt))
    , sinks_(std::move(sinks))
    , last_flush_(std::chrono::steady_clock::now()) {
    batch_.reserve(opt_.batch_size);
}

const std::string* Pipeline::lookup_user(uint32_t uid) {
    const auto it = user_cache_.find(uid);
    if (it != user_cache_.end())
        return it->second.empty() ? nullptr : &it->second;

    std::string name;
    passwd pw{};
    passwd* result = nullptr;
    char buf[4096];
    if (::getpwuid_r(uid, &pw, buf, sizeof(buf), &result) == 0 && result && result->pw_name)
        name = result->pw_name;

    // Negative results are cached too — an empty string — so a uid with no
    // passwd entry does not hit NSS on every single event.
    auto [inserted, _] = user_cache_.emplace(uid, std::move(name));
    return inserted->second.empty() ? nullptr : &inserted->second;
}

void Pipeline::enrich(Record& r) {
    // The probe stamps bpf_ktime_get_ns(), and the resolver's inode-reuse guard
    // compares against that same monotonic clock. Keep it before converting.
    const uint64_t event_monotonic_ns = r.ts_ns;

    // Records carry wall clock so they join against Slurm's accounting database.
    r.ts_ns = event_monotonic_ns + boot_offset_ns();
    r.node = opt_.node;
    r.cluster = opt_.cluster;

    if (r.uid)
        if (const std::string* user = lookup_user(*r.uid))
            r.user = *user;

    if (resolver_ == nullptr)
        return;

    if (auto attr = resolver_->resolve(r.cgroup_id, event_monotonic_ns)) {
        r.job_id = attr->job_id;
        if (!attr->step_id.empty())
            r.step_id = attr->step_id;
        r.task_id = attr->task_id;
    } else {
        ++stats_.unattributed;
        if (opt_.verbose)
            std::cerr << "unattributed cgroup " << r.cgroup_id << "\n";
    }
}

void Pipeline::emit(Record r) {
    enrich(r);
    ++stats_.records;

    batch_.push_back(std::move(r));
    if (batch_.size() >= opt_.batch_size)
        ship();
}

void Pipeline::ship() {
    if (batch_.empty())
        return;

    std::vector<Record> batch;
    batch.swap(batch_);
    batch_.reserve(opt_.batch_size);

    // Every sink gets its own copy; a sink may outlive this call and must not
    // alias another sink's records. The last one takes the original by move.
    for (size_t i = 0; i < sinks_.size(); ++i) {
        if (i + 1 == sinks_.size())
            sinks_[i]->write(std::move(batch));
        else
            sinks_[i]->write(batch);
    }
}

void Pipeline::tick(std::chrono::steady_clock::time_point now) {
    if (now - last_flush_ < opt_.flush_interval)
        return;
    ship();
    last_flush_ = now;
}

void Pipeline::flush() {
    ship();
    for (Sink* s : sinks_)
        s->flush();
    last_flush_ = std::chrono::steady_clock::now();
}

} // namespace slurm_tracer
