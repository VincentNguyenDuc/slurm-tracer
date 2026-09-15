#!/bin/bash
# Submits a plain 2-node job (exercises proc_lifecycle) and records its job id
# and output snapshot to $OUT_DIR/scenarios/proc_lifecycle/. Run by run.sh
# once the cluster is up; not meant to be run standalone.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

log() { echo "scenarios/proc_lifecycle.sh: $*"; }

log "submitting a 2-node job"
JOB_OUTPUT="$(docker compose exec -T ctld srun --nodes=2 --ntasks-per-node=1 --partition=debug --time=2 \
    bash -c 'echo "JOBID=$SLURM_JOB_ID NODE=$SLURMD_NODENAME"; sleep 1; /bin/true')"
echo "$JOB_OUTPUT"

JOB_ID="$(printf '%s\n' "$JOB_OUTPUT" | grep -oE 'JOBID=[0-9]+' | head -1 | cut -d= -f2)"
if [ -z "${JOB_ID:-}" ]; then
    log "could not determine job id from srun output"
    exit 1
fi
log "job id: $JOB_ID"

# flush_interval defaults to 1s (src/core/config.h) and the resolver's inotify
# watch needs a moment to notice the new job cgroup; give both a beat before
# snapshotting output.
sleep 3

record_scenario proc_lifecycle "$JOB_ID" "$JOB_OUTPUT"
