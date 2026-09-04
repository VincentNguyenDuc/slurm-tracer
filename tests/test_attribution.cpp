// Resolver tests.
//
// The resolver's contract is "the inode number of a directory is the cgroup
// id", so these tests build a real directory tree in a temp dir, shaped like
// Slurm's cgroup hierarchy, and stat() it exactly the way the daemon does. That
// exercises the scan, the inotify path, the grace period and the inode-reuse
// guard against the real filesystem rather than a mock.

#include "check.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "attribution.h"

namespace {

std::string make_temp_dir() {
    char tmpl[] = "/tmp/st-test-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (!dir)
        throw std::runtime_error("mkdtemp failed");
    return dir;
}

void mkdirs(const std::string& path) {
    std::string acc;
    for (size_t i = 0; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            if (!acc.empty())
                ::mkdir(acc.c_str(), 0755);
        }
        if (i < path.size())
            acc += path[i];
    }
}

uint64_t inode_of(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0)
        throw std::runtime_error("stat failed: " + path);
    return static_cast<uint64_t>(st.st_ino);
}

void rm_rf(const std::string& path) {
    const std::string cmd = "rm -rf '" + path + "'";
    if (std::system(cmd.c_str()) != 0)
        std::printf("    (warning: cleanup failed for %s)\n", path.c_str());
}

// The layout from docs/DESIGN.md §4.
struct Fixture {
    std::string root;

    Fixture() {
        root = make_temp_dir();
        mkdirs(root + "/system");
        mkdirs(root + "/job_4242/step_batch/user/task_0");
        mkdirs(root + "/job_4242/step_batch/slurm");
        mkdirs(root + "/job_4242/step_0/user/task_0");
        mkdirs(root + "/job_4242/step_0/user/task_1");
        mkdirs(root + "/job_4242/step_extern/user");
        mkdirs(root + "/job_99/step_interactive/user/task_0");
    }
    ~Fixture() { rm_rf(root); }
};

} // namespace

TEST_CASE(parse_path_job_step_task) {
    const auto a = st::parse_cgroup_path("job_4242/step_0/user/task_1");
    REQUIRE(a.has_value());
    CHECK_EQ(a->job_id, 4242u);
    CHECK_EQ(a->step_id, std::string("0"));
    REQUIRE(a->task_id.has_value());
    CHECK_EQ(*a->task_id, 1u);
}

TEST_CASE(parse_path_named_steps) {
    // batch, extern and interactive are steps too, and dashboards need to tell
    // them apart from numbered ones.
    for (const char* step : {"batch", "extern", "interactive"}) {
        const auto a = st::parse_cgroup_path(std::string("job_7/step_") + step);
        REQUIRE(a.has_value());
        CHECK_EQ(a->job_id, 7u);
        CHECK_EQ(a->step_id, std::string(step));
        CHECK(!a->task_id.has_value());
    }
}

TEST_CASE(parse_path_step_without_task) {
    const auto a = st::parse_cgroup_path("job_5/step_2/slurm");
    REQUIRE(a.has_value());
    CHECK_EQ(a->job_id, 5u);
    CHECK_EQ(a->step_id, std::string("2"));
    CHECK(!a->task_id.has_value());
}

TEST_CASE(parse_path_rejects_non_job_paths) {
    // slurmstepd's own cgroup carries no job identity and must not be attributed.
    CHECK(!st::parse_cgroup_path("system").has_value());
    CHECK(!st::parse_cgroup_path("").has_value());
    CHECK(!st::parse_cgroup_path("user.slice/user-1000.slice").has_value());
    // Malformed job segments must not become job 12.
    CHECK(!st::parse_cgroup_path("job_12x/step_0").has_value());
    CHECK(!st::parse_cgroup_path("job_/step_0").has_value());
}

TEST_CASE(parse_path_leading_slash_and_extra_segments) {
    const auto a = st::parse_cgroup_path("/job_1/unexpected/step_3/user/task_9");
    REQUIRE(a.has_value());
    CHECK_EQ(a->job_id, 1u);
    CHECK_EQ(a->step_id, std::string("3"));
    REQUIRE(a->task_id.has_value());
    CHECK_EQ(*a->task_id, 9u);
}

TEST_CASE(resolve_from_initial_scan) {
    Fixture f;
    st::CgroupResolver r(f.root);
    REQUIRE(r.start());

    const uint64_t task1 = inode_of(f.root + "/job_4242/step_0/user/task_1");
    const auto a = r.resolve(task1, st::monotonic_ns());
    REQUIRE(a.has_value());
    CHECK_EQ(a->job_id, 4242u);
    CHECK_EQ(a->step_id, std::string("0"));
    REQUIRE(a->task_id.has_value());
    CHECK_EQ(*a->task_id, 1u);
    CHECK_EQ(r.stats().hits, 1ull);
}

TEST_CASE(resolve_inherits_attribution_from_ancestors) {
    Fixture f;
    st::CgroupResolver r(f.root);
    REQUIRE(r.start());

    // The "user" directory carries no job_/step_ segment of its own, but it
    // sits under one; a process there still belongs to that step.
    const uint64_t user_dir = inode_of(f.root + "/job_4242/step_extern/user");
    const auto a = r.resolve(user_dir, st::monotonic_ns());
    REQUIRE(a.has_value());
    CHECK_EQ(a->job_id, 4242u);
    CHECK_EQ(a->step_id, std::string("extern"));
}

TEST_CASE(resolve_skips_non_job_cgroups) {
    Fixture f;
    st::CgroupResolver r(f.root);
    REQUIRE(r.start());

    const uint64_t system_dir = inode_of(f.root + "/system");
    CHECK(!r.resolve(system_dir, st::monotonic_ns()).has_value());
    CHECK_EQ(r.stats().misses, 1ull);
}

TEST_CASE(resolve_unknown_id_is_a_miss_not_a_crash) {
    Fixture f;
    st::CgroupResolver r(f.root);
    REQUIRE(r.start());

    CHECK(!r.resolve(0xdeadbeefull, st::monotonic_ns()).has_value());
    CHECK_EQ(r.stats().misses, 1ull);
}

TEST_CASE(rescan_on_miss_finds_a_step_created_after_startup) {
    // An event can beat the inotify notification for its own cgroup. Without
    // touching tick(), resolve() must still find a directory created after the
    // initial scan.
    Fixture f;
    st::CgroupResolver r(f.root);
    REQUIRE(r.start());

    const std::string fresh = f.root + "/job_555/step_batch/user/task_0";
    mkdirs(fresh);
    const uint64_t id = inode_of(fresh);

    // The rescan is rate-limited to once per 100 ms; start() already scanned.
    std::this_thread::sleep_for(std::chrono::milliseconds(120));

    const auto a = r.resolve(id, st::monotonic_ns());
    REQUIRE(a.has_value());
    CHECK_EQ(a->job_id, 555u);
    CHECK_EQ(a->step_id, std::string("batch"));
}

TEST_CASE(inotify_picks_up_a_new_step) {
    Fixture f;
    st::CgroupResolver r(f.root);
    REQUIRE(r.start());
    REQUIRE(r.inotify_fd() >= 0);

    const size_t before = r.size();
    mkdirs(f.root + "/job_888");
    mkdirs(f.root + "/job_888/step_0");

    // Give the kernel a moment to queue the notifications, then drain.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r.tick();

    CHECK(r.size() > before);
    const auto a = r.resolve(inode_of(f.root + "/job_888/step_0"), st::monotonic_ns());
    REQUIRE(a.has_value());
    CHECK_EQ(a->job_id, 888u);
    // No rescan was needed: inotify carried it.
    CHECK_EQ(r.stats().rescans, 1ull);
}

TEST_CASE(entry_survives_removal_for_the_grace_period) {
    // Exit events arrive while the cgroup is already being torn down. Losing
    // attribution for them would lose exactly the records that matter most.
    Fixture f;
    const uint64_t grace_ns = 500ull * 1000000ull;
    st::CgroupResolver r(f.root, grace_ns);
    REQUIRE(r.start());

    const std::string step = f.root + "/job_99/step_interactive/user/task_0";
    const uint64_t id = inode_of(step);
    REQUIRE(r.resolve(id, st::monotonic_ns()).has_value());

    ::rmdir(step.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r.tick();

    const auto a = r.resolve(id, st::monotonic_ns());
    REQUIRE(a.has_value());
    CHECK_EQ(a->job_id, 99u);
}

TEST_CASE(entry_is_dropped_after_the_grace_period) {
    Fixture f;
    const uint64_t grace_ns = 100ull * 1000000ull; // 100 ms
    st::CgroupResolver r(f.root, grace_ns);
    REQUIRE(r.start());

    const std::string step = f.root + "/job_99/step_interactive/user/task_0";
    const uint64_t id = inode_of(step);
    REQUIRE(r.resolve(id, st::monotonic_ns()).has_value());

    ::rmdir(step.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    r.tick(); // marks removed
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    r.tick(); // past the grace period

    // The directory is gone, so a rescan cannot bring it back either.
    CHECK(!r.resolve(id, st::monotonic_ns()).has_value());
}

TEST_CASE(inode_reuse_guard_rejects_an_event_older_than_its_entry) {
    // A recycled cgroup id must not inherit the previous job's identity: an
    // event stamped before the entry was created cannot belong to it.
    Fixture f;
    st::CgroupResolver r(f.root);
    REQUIRE(r.start());

    const uint64_t id = inode_of(f.root + "/job_4242/step_batch/user/task_0");
    const uint64_t long_ago = 1; // monotonic ns near boot

    CHECK(!r.resolve(id, long_ago).has_value());
    CHECK_EQ(r.stats().stale, 1ull);

    // A current event against the same entry still resolves.
    CHECK(r.resolve(id, st::monotonic_ns()).has_value());
    CHECK_EQ(r.stats().hits, 1ull);
}

TEST_CASE(start_fails_on_a_missing_root) {
    st::CgroupResolver r("/nonexistent/slurm/cgroup/root");
    CHECK(!r.start());
}

TEST_CASE(discover_honours_an_explicit_override) {
    Fixture f;
    const auto root = st::discover_cgroup_root(f.root);
    REQUIRE(root.has_value());
    CHECK_EQ(*root, f.root);

    CHECK(!st::discover_cgroup_root("/nonexistent/path").has_value());
}

TEST_CASE(discover_finds_the_versioned_stepd_scope) {
    // Verified against slurmd 22.05.8, which logs:
    //   Could not create scope directory
    //   /sys/fs/cgroup/system.slice/<node>_slurmstepd.scope
    // The node name comes first, so a pattern anchored on "slurmstepd*.scope"
    // would find nothing on a real node.
    Fixture f;
    mkdirs(f.root + "/system.slice/cn01_slurmstepd.scope");

    const std::string conf = f.root + "/cgroup.conf";
    if (std::FILE* fp = std::fopen(conf.c_str(), "w")) {
        std::fprintf(fp, "CgroupPlugin=cgroup/v2\nCgroupMountpoint=%s\n", f.root.c_str());
        std::fclose(fp);
    }

    const auto root = st::discover_cgroup_root("", conf);
    REQUIRE(root.has_value());
    CHECK_EQ(*root, f.root + "/system.slice/cn01_slurmstepd.scope");
}

TEST_CASE(discover_finds_a_bare_stepd_scope) {
    // Other releases spell it without the node prefix; both must resolve.
    Fixture f;
    mkdirs(f.root + "/system.slice/slurmstepd.scope");

    const std::string conf = f.root + "/cgroup.conf";
    if (std::FILE* fp = std::fopen(conf.c_str(), "w")) {
        std::fprintf(fp, "CgroupMountpoint=%s\n", f.root.c_str());
        std::fclose(fp);
    }

    const auto root = st::discover_cgroup_root("", conf);
    REQUIRE(root.has_value());
    CHECK_EQ(*root, f.root + "/system.slice/slurmstepd.scope");
}

TEST_CASE(parse_path_task_special_inherits_the_step) {
    // Slurm puts tasks it cannot number into task_special. That is not a task
    // id, so the record should carry the step with no task rather than a
    // bogus one.
    const auto a = st::parse_cgroup_path("job_3/step_0/user/task_special");
    REQUIRE(a.has_value());
    CHECK_EQ(a->job_id, 3u);
    CHECK_EQ(a->step_id, std::string("0"));
    CHECK(!a->task_id.has_value());
}

TEST_CASE(discover_reads_cgroup_conf) {
    // IgnoreSystemd=yes drops the system.slice/...scope prefix, and
    // CgroupMountpoint relocates the root. Both are config, not constants.
    Fixture f;
    mkdirs(f.root + "/slurm");

    const std::string conf = f.root + "/cgroup.conf";
    if (std::FILE* fp = std::fopen(conf.c_str(), "w")) {
        std::fprintf(fp, "# a comment\n");
        std::fprintf(fp, "CgroupMountpoint=%s\n", f.root.c_str());
        std::fprintf(fp, "IgnoreSystemd=yes\n");
        std::fclose(fp);
    }

    const auto root = st::discover_cgroup_root("", conf);
    REQUIRE(root.has_value());
    CHECK_EQ(*root, f.root + "/slurm");
}
