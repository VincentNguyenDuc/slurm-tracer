#!/bin/bash
set -euo pipefail
source /opt/st/scripts/common.sh

NODE="$(hostname)"

# Must run before anything else starts a background process (munged
# included): the delegation only works while this shell is the sole process
# sitting in the root cgroup (cgroup v2's "no internal process" rule).
setup_cgroup_delegation
setup_munge
mkdir -p /var/spool/slurmd /var/log/slurm /var/log/slurm-tracer
wait_for_binary

slurmd -D -vv &
SLURMD_PID=$!

# slurm-tracer's discover_cgroup_root() (src/core/attribution.cpp) only runs
# once, at startup, and daemon.cpp now treats "not found yet" as fatal instead
# of retrying (commit 7e4fa1c) -- so starting the tracer before slurmd has
# actually created its cgroup scope just kills it, silently, with nothing
# here noticing. Poll for the real precondition instead of guessing a fixed
# delay; patterns mirror discover_cgroup_root's exactly.
wait_for_cgroup_root() {
    local patterns=(
        '/sys/fs/cgroup/system.slice/*slurmstepd*.scope'
        '/sys/fs/cgroup/*.slice/*slurmstepd*.scope'
        '/sys/fs/cgroup/slurm'
    )
    for _ in $(seq 1 50); do
        local pattern
        for pattern in "${patterns[@]}"; do
            compgen -G "$pattern" >/dev/null && return 0
        done
        sleep 0.2
    done
    echo "entrypoint-worker: slurmd never created its cgroup scope; not starting slurm-tracer" >&2
    return 1
}
wait_for_cgroup_root

# slurm-tracer runs for the whole container lifetime, independent of any one
# job, same as it would on a real compute node: it watches the cgroup tree and
# attributes whatever jobs land on it. --cgroup-root is left to auto-discovery
# (src/core/attribution.cpp) rather than pinned, so this exercises the same
# discovery path a real deployment relies on.
#
# conf/tracer.toml (identical on both workers) picks the probes and sinks,
# including the http sink shipping to the collector service -- --node is
# passed after --config specifically to also exercise that a flag on the
# command line overrides what the file set (src/main.cpp).
#
# Started after slurmd so slurmd is always up first.
/workspace/build/docker/slurm-tracer \
    --config /etc/slurm-tracer/config.toml \
    --node "$NODE" \
    --verbose \
    >>"/var/log/slurm-tracer/${NODE}.jsonl" 2>>"/var/log/slurm-tracer/${NODE}.log" &
TRACER_PID=$!

term() {
    kill -TERM "$SLURMD_PID" 2>/dev/null || true
    wait "$SLURMD_PID" 2>/dev/null || true
    kill -TERM "$TRACER_PID" 2>/dev/null || true
    wait "$TRACER_PID" 2>/dev/null || true
}
trap term TERM INT

# Either process exiting unexpectedly must bring the container down, not just
# slurmd -- a tracer that dies (e.g. the cgroup-root fail-fast above) must not
# leave slurmd running unnoticed with nothing attributing jobs. run.sh's
# wait_for_idle already treats a service exiting early as fatal to the whole
# cluster run, so surfacing that here is what lets it catch this promptly.
set +e
wait -n "$SLURMD_PID" "$TRACER_PID"
rc=$?
set -e
term
exit "$rc"
