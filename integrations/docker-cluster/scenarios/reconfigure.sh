#!/bin/bash
# Exercises SIGHUP reload (Daemon::reload_config): rewrites c1's tracer
# config in place -- dropping the proc_lifecycle probe and the http sink --
# sends SIGHUP, and confirms the reload happened before submitting a job that
# should still reach the surviving oom probe and stdout_json sink. Restores
# c1's original config and reloads it back once done, so this is the only
# scenario that mutates a node's running config and must therefore run last;
# it relies on alphabetical scenario ordering (run.sh) to sort after
# oom.sh/proc_lifecycle.sh.
#
# conf/tracer_c1.json is bind-mounted read-only into the c1 container, but
# it's an ordinary host file underneath that mount, so the host side (here)
# can still rewrite it -- exactly what an operator editing a node's config
# before sending SIGHUP would do.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

log() { echo "scenarios/reconfigure.sh: $*"; }

CONF="conf/tracer_c1.json"
BACKUP="live/tracer_c1.json.orig"
cp "$CONF" "$BACKUP"

restore() {
    mv "$BACKUP" "$CONF"
    docker compose exec -T c1 pkill -HUP -f slurm-tracer >/dev/null 2>&1 || true
}
trap restore EXIT

wait_for_reload() {
    local tries=30
    for _ in $(seq 1 "$tries"); do
        docker compose exec -T c1 grep -q 'reload: config re-read from' \
            /var/log/slurm-tracer/c1.log 2>/dev/null && return 0
        sleep 1
    done
    log "c1 never logged a reload; tracer log tail:"
    docker compose exec -T c1 tail -n 50 /var/log/slurm-tracer/c1.log || true
    return 1
}

log "dropping proc_lifecycle probe and http sink from c1's config, then sending SIGHUP"
cat > "$CONF" <<'EOF'
{
    "node": { "cluster": "docker-test", "node": "c1" },
    "probes": { "oom": {} },
    "sinks": { "stdout_json": {} }
}
EOF
docker compose exec -T c1 pkill -HUP -f slurm-tracer
wait_for_reload
log "reload confirmed on c1"

# batch_size/flush_interval are unchanged by this reload (reload_config only
# diffs probes/sinks by name -- see daemon.cpp), but give remove_sink's flush
# and the new wiring a moment to settle before generating traffic anyway.
sleep 1

log "submitting a job pinned to c1 that still exercises the surviving oom probe"
JOB_OUTPUT="$(docker compose exec -T ctld srun --nodes=1 --ntasks=1 --nodelist=c1 --partition=debug --time=1 --mem=50M \
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

record_scenario reconfigure "$JOB_ID" "$JOB_OUTPUT"
