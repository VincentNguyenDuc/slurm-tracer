#!/bin/bash
# Submits a deliberately oversubscribed 2-node workload to give sched_latency
# (an aggregating probe -- see docs/DESIGN.md §6) something to actually
# bucket, and records its job id and output snapshot to
# $OUT_DIR/scenarios/sched_latency/. Run by run.sh once the cluster is up;
# not meant to be run standalone.
#
# `srun --overcommit` only oversubscribes a *step* beyond its allocation's CPU
# count, not a fresh allocation request -- asking a plain srun for more tasks
# than the node has CPUs fails allocation outright ("never runnable in
# partition"), --overcommit or not. So this allocates 2 nodes at their real
# (1 CPU each) size with salloc, then runs the oversubscribed step inside that
# allocation. Each of the 32 tasks per node spins on the CPU for a couple of
# seconds, so real run-queue contention happens regardless of how many cores
# the host running this actually has.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

log() { echo "scenarios/sched_latency.sh: $*"; }

log "submitting an oversubscribed 2-node workload"
JOB_OUTPUT="$(docker compose exec -T ctld bash -c '
    salloc --nodes=2 --partition=debug --time=1 \
        srun --overcommit --ntasks-per-node=32 \
            bash -c "echo JOBID=\$SLURM_JOB_ID NODE=\$SLURMD_NODENAME; timeout 2 yes >/dev/null"
' 2>&1 || true)"
echo "$JOB_OUTPUT"

JOB_ID="$(printf '%s\n' "$JOB_OUTPUT" | grep -oE 'JOBID=[0-9]+' | head -1 | cut -d= -f2)"
if [ -z "${JOB_ID:-}" ]; then
    log "could not determine job id for the oversubscribed workload"
    exit 1
fi
log "job id: $JOB_ID"

# sched_latency is flushed on config_.flush_interval (default 1s, see
# src/daemon.cpp's poll_probes()); give it a couple of intervals after the
# job finishes before snapshotting output.
sleep 3

record_scenario sched_latency "$JOB_ID" "$JOB_OUTPUT"
