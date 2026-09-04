// cgroup id -> Slurm job/step/task, per docs/DESIGN.md §4.
//
// Probes stamp bpf_get_current_cgroup_id() on every event. That id is the inode
// number of the cgroup directory, so userspace can resolve it by walking the
// Slurm cgroup tree and stat()ing each directory. All of the Slurm-layout
// knowledge lives here; the kernel side knows nothing about it.

#ifndef SLURM_TRACER_ATTRIBUTION_H
#define SLURM_TRACER_ATTRIBUTION_H

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace st {

struct Attribution {
    uint32_t job_id = 0;
    std::string step_id;             // "0", "batch", "extern", "interactive"
    std::optional<uint32_t> task_id; // set only for .../task_<n>
};

// Parses a path relative to the cgroup root into an attribution.
//
// Recognises job_<id>, step_<id> and task_<id> segments and ignores Slurm's
// structural segments ("user", "slurm", "system"). Returns nullopt when the
// path carries no job_ segment at all — slurmstepd's own cgroup, for instance.
std::optional<Attribution> parse_cgroup_path(const std::string& relative_path);

// Finds the Slurm cgroup root. Honours, in order: an explicit override, then
// CgroupMountpoint/IgnoreSystemd from cgroup.conf, then the known layouts.
// Returns nullopt when nothing plausible exists — a node where slurmd has never
// run, typically.
std::optional<std::string> discover_cgroup_root(
    const std::string& override_path = {}, const std::string& cgroup_conf = "/etc/slurm/cgroup.conf"
);

class CgroupResolver {
public:
    // `grace_ns` keeps an entry alive after its cgroup directory disappears:
    // exit events routinely arrive while the cgroup is already being torn down.
    explicit CgroupResolver(std::string root, uint64_t grace_ns = 30ull * 1000000000ull);
    ~CgroupResolver();

    CgroupResolver(const CgroupResolver&) = delete;
    CgroupResolver& operator=(const CgroupResolver&) = delete;

    // Populates the map from the tree as it stands and arms inotify. Returns
    // false only if the root cannot be read at all.
    bool start();

    // Resolves a cgroup id. On a miss, rescans the tree once before giving up,
    // because an event can beat the inotify notification that created its
    // cgroup. `event_ts_ns` is the event's CLOCK_MONOTONIC timestamp and is
    // used to reject entries created after the event — the inode-reuse guard.
    std::optional<Attribution> resolve(uint64_t cgroup_id, uint64_t event_ts_ns);

    // Drains pending inotify notifications and expires entries past the grace
    // period. Cheap; call it once per poll iteration.
    void tick();

    // File descriptor to add to an event loop, or -1 when inotify is not armed.
    int inotify_fd() const { return inotify_fd_; }

    size_t size() const { return entries_.size(); }

    // Counters, exported so a misconfigured resolver is visible rather than
    // silently attributing nothing.
    struct Stats {
        uint64_t hits = 0;
        uint64_t misses = 0; // unresolved even after a rescan
        uint64_t rescans = 0;
        uint64_t stale = 0; // rejected by the inode-reuse guard
    };
    const Stats& stats() const { return stats_; }

private:
    struct Entry {
        Attribution attr;
        std::string path;
        uint64_t created_ns = 0; // CLOCK_MONOTONIC, when we first saw it
        uint64_t removed_ns = 0; // 0 while live, else start of the grace period
    };

    void scan();
    void add_subtree(const std::string& dir);
    void add_dir(const std::string& abs_path);
    void watch_dir(const std::string& abs_path);
    void drain_inotify();
    void expire();

    std::string root_;
    uint64_t grace_ns_;
    int inotify_fd_ = -1;
    std::unordered_map<uint64_t, Entry> entries_;
    std::unordered_map<int, std::string> watches_; // inotify wd -> path
    uint64_t last_rescan_ns_ = 0;
    Stats stats_;
};

// CLOCK_MONOTONIC nanoseconds — the same clock as bpf_ktime_get_ns().
uint64_t monotonic_ns();

} // namespace st

#endif // SLURM_TRACER_ATTRIBUTION_H
