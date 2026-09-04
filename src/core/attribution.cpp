#include "core/attribution.h"

#include <dirent.h>
#include <glob.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <vector>

namespace slurm_tracer {
namespace {

// A rescan is bounded to once per this interval so that a burst of events for
// an unknown cgroup cannot turn into a walk of the tree per event.
constexpr uint64_t kRescanMinIntervalNs = 100ull * 1000000ull; // 100 ms

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start < path.size()) {
        const size_t slash = path.find('/', start);
        const size_t end = (slash == std::string::npos) ? path.size() : slash;
        if (end > start)
            parts.emplace_back(path, start, end - start);
        start = end + 1;
    }
    return parts;
}

// Parses "<prefix><digits>" into the number. Rejects anything with trailing
// junk so "job_12x" does not silently become job 12.
std::optional<uint32_t> parse_numeric_suffix(const std::string& seg, const char* prefix) {
    const size_t plen = std::strlen(prefix);
    if (seg.size() <= plen || seg.compare(0, plen, prefix) != 0)
        return std::nullopt;

    const std::string digits = seg.substr(plen);
    if (digits.empty())
        return std::nullopt;
    for (const char c : digits) {
        if (c < '0' || c > '9')
            return std::nullopt;
    }

    errno = 0;
    const unsigned long value = std::strtoul(digits.c_str(), nullptr, 10);
    if (errno != 0 || value > UINT32_MAX)
        return std::nullopt;
    return static_cast<uint32_t>(value);
}

bool is_dir(const std::string& path) {
    struct stat slurm_tracer {};
    return ::stat(path.c_str(), &slurm_tracer) == 0 && S_ISDIR(slurm_tracer.st_mode);
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

} // namespace

uint64_t monotonic_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

namespace {

// CLOCK_REALTIME - CLOCK_MONOTONIC, sampled once. Lets us express a file's
// ctime (realtime) on the same clock as a BPF event's timestamp (monotonic).
uint64_t boot_offset_ns() {
    static const uint64_t offset = [] {
        timespec rt{};
        clock_gettime(CLOCK_REALTIME, &rt);
        const uint64_t realtime =
            static_cast<uint64_t>(rt.tv_sec) * 1000000000ull + static_cast<uint64_t>(rt.tv_nsec);
        return realtime - monotonic_ns();
    }();
    return offset;
}

// When the cgroup directory was created, on CLOCK_MONOTONIC.
//
// This must be the directory's own creation time, not the moment we happened to
// notice it. We routinely discover a cgroup *because* an event from it arrived
// and missed; timestamping the entry with "now" would make every such entry
// look newer than the event that found it, and the reuse guard below would
// reject them all.
uint64_t created_from_ctime(const struct stat& slurm_tracer) {
    const uint64_t ctime_realtime =
        static_cast<uint64_t>(slurm_tracer.st_ctim.tv_sec) * 1000000000ull +
        static_cast<uint64_t>(slurm_tracer.st_ctim.tv_nsec);
    const uint64_t offset = boot_offset_ns();
    return ctime_realtime > offset ? ctime_realtime - offset : 0;
}

} // namespace

std::optional<Attribution> parse_cgroup_path(const std::string& relative_path) {
    Attribution attr;
    bool saw_job = false;

    for (const std::string& seg : split_path(relative_path)) {
        if (const auto job = parse_numeric_suffix(seg, "job_")) {
            attr.job_id = *job;
            saw_job = true;
            // A nested job_ segment would mean a layout we do not understand;
            // reset the finer-grained fields rather than mixing two jobs.
            attr.step_id.clear();
            attr.task_id.reset();
        } else if (seg.rfind("step_", 0) == 0 && seg.size() > 5) {
            // Step ids are not always numeric: batch, extern and interactive
            // are steps too, and the dashboards want to tell them apart.
            attr.step_id = seg.substr(5);
            attr.task_id.reset();
        } else if (const auto task = parse_numeric_suffix(seg, "task_")) {
            attr.task_id = *task;
        }
        // Structural segments ("user", "slurm", "system") and anything else
        // are skipped: they refine nothing, and the attribution accumulated
        // from the ancestors still applies.
    }

    if (!saw_job)
        return std::nullopt;
    return attr;
}

std::optional<std::string> discover_cgroup_root(
    const std::string& override_path, const std::string& cgroup_conf
) {
    if (!override_path.empty()) {
        if (is_dir(override_path))
            return override_path;
        std::cerr << "configured cgroup root does not exist: " << override_path << "\n";
        return std::nullopt;
    }

    // cgroup.conf moves the root two ways: CgroupMountpoint relocates it, and
    // IgnoreSystemd=yes drops the system.slice/...scope prefix entirely.
    std::string mountpoint = "/sys/fs/cgroup";
    bool ignore_systemd = false;
    if (std::ifstream conf{cgroup_conf}) {
        std::string line;
        while (std::getline(conf, line)) {
            const std::string t = trim(line);
            if (t.empty() || t[0] == '#')
                continue;
            const size_t eq = t.find('=');
            if (eq == std::string::npos)
                continue;

            std::string key = trim(t.substr(0, eq));
            const std::string value = trim(t.substr(eq + 1));
            for (char& c : key)
                c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));

            if (key == "cgroupmountpoint" && !value.empty())
                mountpoint = value;
            else if (key == "ignoresystemd")
                ignore_systemd = (value == "yes" || value == "Yes" || value == "YES");
        }
    }

    // Slurm 22.05 names the scope "<node>_slurmstepd.scope" — the node name
    // comes *first* — and the spelling has moved across releases. Hence a
    // pattern with wildcards on both sides rather than a fixed name; anchoring
    // on "slurmstepd*.scope" finds nothing on a real 22.05 node.
    //
    // IgnoreSystemd only changes who creates that directory (systemd over dbus,
    // or slurmstepd with mkdir), not where it is; the bare <mountpoint>/slurm
    // candidate covers layouts where it does move.
    (void)ignore_systemd;
    const std::vector<std::string> patterns = {
        mountpoint + "/system.slice/*slurmstepd*.scope",
        mountpoint + "/*.slice/*slurmstepd*.scope",
        mountpoint + "/slurm",
    };

    for (const std::string& pattern : patterns) {
        glob_t g{};
        if (::glob(pattern.c_str(), GLOB_ONLYDIR | GLOB_NOSORT, nullptr, &g) == 0) {
            std::optional<std::string> found;
            if (g.gl_pathc > 0)
                found = g.gl_pathv[0];
            ::globfree(&g);
            if (found)
                return found;
        }
    }
    return std::nullopt;
}

CgroupResolver::CgroupResolver(std::string root, uint64_t grace_ns)
    : root_(std::move(root))
    , grace_ns_(grace_ns) {}

CgroupResolver::~CgroupResolver() {
    if (inotify_fd_ >= 0)
        ::close(inotify_fd_);
}

bool CgroupResolver::start() {
    if (!is_dir(root_)) {
        std::cerr << "cgroup root is not a directory: " << root_ << "\n";
        return false;
    }

    // inotify is the fast path for new steps. It is not required for
    // correctness — resolve() rescans on a miss — so a failure here degrades
    // rather than aborts.
    inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd_ < 0)
        std::cerr << "inotify_init1 failed (" << std::strerror(errno)
                  << "); falling back to rescan-on-miss\n";

    scan();
    return true;
}

void CgroupResolver::add_dir(const std::string& abs_path) {
    struct stat slurm_tracer {};
    if (::stat(abs_path.c_str(), &slurm_tracer) != 0)
        return;

    // The cgroup id *is* the directory's inode number. This is the entire
    // bridge between the kernel-side stamp and Slurm's job identity.
    const uint64_t cgroup_id = static_cast<uint64_t>(slurm_tracer.st_ino);

    const std::string relative =
        abs_path.size() > root_.size() ? abs_path.substr(root_.size() + 1) : std::string{};
    auto attr = parse_cgroup_path(relative);
    if (!attr)
        return; // not job work — slurmstepd's own cgroup, for instance

    const uint64_t created_ns = created_from_ctime(slurm_tracer);

    auto it = entries_.find(cgroup_id);
    if (it != entries_.end()) {
        // Already known. A different path, or a resurrected entry, means the
        // inode was recycled: adopt the new identity and its creation time so
        // the reuse guard measures against the *new* cgroup.
        if (it->second.removed_ns != 0 || it->second.path != abs_path) {
            it->second.attr = *attr;
            it->second.path = abs_path;
            it->second.created_ns = created_ns;
            it->second.removed_ns = 0;
        }
        return;
    }

    Entry e;
    e.attr = *attr;
    e.path = abs_path;
    e.created_ns = created_ns;
    entries_.emplace(cgroup_id, std::move(e));
}

// Walks `dir` and everything under it, recording and watching each directory.
//
// Adding an inotify watch is inherently racy: children created between the
// mkdir and inotify_add_watch never fire IN_CREATE. Enumerating the subtree
// immediately after arming the watch is what closes that hole, so every newly
// created directory goes through here rather than through watch_dir() alone.
void CgroupResolver::add_subtree(const std::string& dir) {
    std::vector<std::string> stack{dir};
    while (!stack.empty()) {
        const std::string current = std::move(stack.back());
        stack.pop_back();

        watch_dir(current);
        if (current != root_)
            add_dir(current);

        DIR* d = ::opendir(current.c_str());
        if (!d)
            continue;
        while (dirent* ent = ::readdir(d)) {
            if (ent->d_name[0] == '.' &&
                (ent->d_name[1] == '\0' || (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
                continue;

            const std::string child = current + "/" + ent->d_name;
            // cgroupfs reports DT_UNKNOWN for its entries on some kernels, so
            // fall back to stat() rather than trusting d_type.
            if (ent->d_type == DT_DIR || (ent->d_type == DT_UNKNOWN && is_dir(child)))
                stack.push_back(child);
        }
        ::closedir(d);
    }
}

void CgroupResolver::watch_dir(const std::string& abs_path) {
    if (inotify_fd_ < 0)
        return;
    const int wd = ::inotify_add_watch(inotify_fd_, abs_path.c_str(), IN_CREATE | IN_DELETE);
    if (wd >= 0)
        watches_[wd] = abs_path;
}

void CgroupResolver::scan() {
    add_subtree(root_);
    last_rescan_ns_ = monotonic_ns();
    ++stats_.rescans;
}

void CgroupResolver::drain_inotify() {
    if (inotify_fd_ < 0)
        return;

    // inotify_event is variable-length; the buffer must hold at least one
    // maximum-length event.
    alignas(inotify_event) char buf[8192];
    for (;;) {
        const ssize_t n = ::read(inotify_fd_, buf, sizeof(buf));
        if (n <= 0)
            return; // EAGAIN when drained

        for (ssize_t off = 0; off < n;) {
            const auto* ev = reinterpret_cast<const inotify_event*>(buf + off);
            off += static_cast<ssize_t>(sizeof(inotify_event) + ev->len);

            if (!(ev->mask & IN_ISDIR) || ev->len == 0)
                continue;
            const auto wit = watches_.find(ev->wd);
            if (wit == watches_.end())
                continue;

            const std::string path = wit->second + "/" + ev->name;
            if (ev->mask & IN_CREATE) {
                // Not just this directory: slurmstepd creates the step, user
                // and task levels back to back, and the deeper ones are already
                // in place by the time this notification is handled.
                add_subtree(path);
            } else if (ev->mask & IN_DELETE) {
                // Start the grace period rather than dropping the entry: exit
                // events for this cgroup are very likely still in flight.
                for (auto& [id, entry] : entries_) {
                    if (entry.path == path && entry.removed_ns == 0)
                        entry.removed_ns = monotonic_ns();
                }
            }
        }
    }
}

void CgroupResolver::expire() {
    const uint64_t now = monotonic_ns();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.removed_ns != 0 && now - it->second.removed_ns > grace_ns_)
            it = entries_.erase(it);
        else
            ++it;
    }
}

void CgroupResolver::tick() {
    drain_inotify();
    expire();
}

std::optional<Attribution> CgroupResolver::resolve(uint64_t cgroup_id, uint64_t event_ts_ns) {
    auto it = entries_.find(cgroup_id);

    if (it == entries_.end()) {
        // An event can beat the inotify notification for the cgroup it came
        // from. Rescan once — rate-limited — before declaring a miss.
        const uint64_t now = monotonic_ns();
        if (now - last_rescan_ns_ >= kRescanMinIntervalNs) {
            scan();
            it = entries_.find(cgroup_id);
        }
        if (it == entries_.end()) {
            ++stats_.misses;
            return std::nullopt;
        }
    }

    // Inode-reuse guard: cgroup ids are inode numbers and the kernel recycles
    // them. An event stamped before this entry was created cannot belong to it,
    // so it must not inherit the new job's identity.
    if (event_ts_ns != 0 && event_ts_ns < it->second.created_ns) {
        ++stats_.stale;
        return std::nullopt;
    }

    ++stats_.hits;
    return it->second.attr;
}

} // namespace slurm_tracer
