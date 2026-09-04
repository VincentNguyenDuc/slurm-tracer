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

# slurm-tracer runs for the whole container lifetime, independent of any one
# job, same as it would on a real compute node: it watches the cgroup tree and
# attributes whatever jobs land on it. --cgroup-root is left to auto-discovery
# (src/core/attribution.cpp) rather than pinned, so this exercises the same
# discovery path a real deployment relies on.
/workspace/build/docker/slurm-tracer \
    --cluster docker-test \
    --node "$NODE" \
    --probes proc_lifecycle,sched_latency \
    --sinks stdout_json \
    --verbose \
    >>"/var/log/slurm-tracer/${NODE}.jsonl" 2>>"/var/log/slurm-tracer/${NODE}.log" &
TRACER_PID=$!

slurmd -D -vv &
SLURMD_PID=$!

term() {
    kill -TERM "$SLURMD_PID" 2>/dev/null || true
    wait "$SLURMD_PID" 2>/dev/null || true
    kill -TERM "$TRACER_PID" 2>/dev/null || true
    wait "$TRACER_PID" 2>/dev/null || true
}
trap term TERM INT

wait "$SLURMD_PID"
term
