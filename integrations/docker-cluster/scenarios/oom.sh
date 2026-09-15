#!/bin/bash
# Submits a 1-node job that deliberately exceeds its own --mem, exercising the
# oom probe (src/probes/oom): the kernel's memcg OOM killer fires inside that
# job's own cgroup once it crosses memory.max -- ConstrainRAMSpace/
# ConstrainSwapSpace=yes in conf/cgroup.conf is what makes --mem become that
# limit in the first place. Records its job id and output snapshot to
# $OUT_DIR/scenarios/oom/. Run by run.sh once the cluster is up; not meant to
# be run standalone.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

log() { echo "scenarios/oom.sh: $*"; }

log "submitting a job that allocates past its --mem limit"
# The list comprehension keeps growing 1 MiB bytearrays (each one actually
# zero-filled, so every page is touched and charged to the cgroup, not just
# reserved) well past the 50M limit -- guaranteed to get OOM-killed long
# before the range() bound is ever reached. srun's own exit status here
# reflects the kill, not a script failure -- captured, not treated as one.
JOB_OUTPUT="$(docker compose exec -T ctld srun --nodes=1 --ntasks=1 --partition=debug --time=1 --mem=50M \
    bash -c 'echo "JOBID=$SLURM_JOB_ID NODE=$SLURMD_NODENAME"; python3 -c "[bytearray(2**20) for _ in range(100000)]"' \
    2>&1 || true)"
echo "$JOB_OUTPUT"

JOB_ID="$(printf '%s\n' "$JOB_OUTPUT" | grep -oE 'JOBID=[0-9]+' | head -1 | cut -d= -f2)"
if [ -z "${JOB_ID:-}" ]; then
    log "could not determine job id from srun output"
    exit 1
fi
log "job id: $JOB_ID"

# flush_interval defaults to 1s (src/core/config.h); give it a couple of
# intervals after the kill before snapshotting output.
sleep 3

record_scenario oom "$JOB_ID" "$JOB_OUTPUT"
