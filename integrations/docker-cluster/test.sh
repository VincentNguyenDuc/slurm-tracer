#!/bin/bash
# Integration test: brings up a two-node Slurm cluster in docker compose, runs
# slurm-tracer as a daemon on each worker, submits a job that spans both
# nodes, and checks that the exec/exit events slurm-tracer captured are
# attributed to that job's id.
#
# Requires a Linux kernel with BTF (/sys/kernel/btf/vmlinux) and cgroup v2
# reachable from Docker -- true of a normal Docker Desktop or native Linux
# Docker install. Workers run --privileged for CAP_BPF/CAP_PERFMON and cgroup
# management; see docker-compose.yml.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

KEEP_UP=0
for arg in "$@"; do
    case "$arg" in
        --keep-up) KEEP_UP=1 ;;
        *)
            echo "usage: $0 [--keep-up]" >&2
            exit 2
            ;;
    esac
done

NODES=(c1 c2)

cleanup() {
    local status=$?
    if [ "$KEEP_UP" -eq 1 ]; then
        echo "test.sh: --keep-up set, leaving the cluster running (docker compose down when done)"
        return
    fi
    echo "test.sh: tearing down"
    docker compose down -v --remove-orphans >/dev/null 2>&1 || true
    exit $status
}
trap cleanup EXIT

log() { echo "test.sh: $*"; }

mkdir -p secrets out/c1 out/c2
if [ ! -s secrets/munge.key ]; then
    log "generating munge key"
    head -c 1024 /dev/urandom > secrets/munge.key
fi
: > out/c1/c1.jsonl
: > out/c2/c2.jsonl

log "building the cluster image"
docker compose build >/dev/null

log "building slurm-tracer (needs the running kernel's BTF; see scripts/build.sh)"
docker compose run --rm builder

log "starting controller and workers"
docker compose up -d ctld c1 c2

wait_for_idle() {
    local tries=60
    for i in $(seq 1 "$tries"); do
        for svc in ctld c1 c2; do
            if [ "$(docker compose ps -q "$svc" | xargs -r docker inspect -f '{{.State.Running}}' 2>/dev/null)" != "true" ]; then
                log "$svc exited early; last logs:"
                docker compose logs "$svc" | tail -50
                return 1
            fi
        done

        local states
        states="$(docker compose exec -T ctld sinfo -h -N -o '%N %T' 2>/dev/null || true)"
        local n
        n="$(printf '%s\n' "$states" | grep -c . || true)"
        if [ "$n" -ge "${#NODES[@]}" ] && ! printf '%s\n' "$states" | awk '{print $2}' | grep -qv '^idle$'; then
            log "cluster ready:"
            printf '%s\n' "$states"
            return 0
        fi
        [ $((i % 5)) -eq 0 ] && log "waiting for nodes to register ($i/${tries}): ${states:-<none yet>}"
        sleep 2
    done
    log "cluster never reached idle; sinfo/scontrol state:"
    docker compose exec -T ctld sinfo -N -l || true
    docker compose exec -T ctld scontrol show nodes || true
    return 1
}
wait_for_idle

# slurm-tracer retries cgroup-root discovery every 5s (src/daemon.cpp,
# kDiscoveryRetry) and only starts attributing once it finds the scope
# directory slurmd creates. Nodes going "idle" in sinfo says nothing about
# that -- wait for each tracer to say so itself, or a job submitted too early
# comes back with every record unattributed.
wait_for_attribution() {
    local tries=30
    for i in $(seq 1 "$tries"); do
        local ready=1
        for node in "${NODES[@]}"; do
            docker compose exec -T "$node" grep -q 'cgroup root appeared' "/var/log/slurm-tracer/${node}.log" 2>/dev/null || ready=0
        done
        [ "$ready" -eq 1 ] && return 0
        sleep 1
    done
    log "slurm-tracer never reported a cgroup root; tracer logs:"
    for node in "${NODES[@]}"; do
        docker compose exec -T "$node" cat "/var/log/slurm-tracer/${node}.log" 2>&1 || true
    done
    return 1
}
wait_for_attribution
# "cgroup root appeared" only means the resolver found the scope directory
# and armed its top-level inotify watch; give it a moment to settle before a
# job creates the nested job_/step_/task_ directories it needs to catch.
sleep 3

log "submitting a 2-node job"
JOB_OUTPUT="$(docker compose exec -T ctld srun --nodes=2 --ntasks-per-node=1 --partition=debug --time=2 \
    bash -c 'echo "JOBID=$SLURM_JOB_ID NODE=$SLURMD_NODENAME"; sleep 1; /bin/true')"
echo "$JOB_OUTPUT"

JOB_ID="$(printf '%s\n' "$JOB_OUTPUT" | grep -oE 'JOBID=[0-9]+' | head -1 | cut -d= -f2)"
if [ -z "${JOB_ID:-}" ]; then
    log "FAIL: could not determine job id from srun output"
    exit 1
fi
log "job id: $JOB_ID"

# flush_interval defaults to 1s (src/core/config.h) and the resolver's inotify
# watch needs a moment to notice the new job cgroup; give both a beat.
sleep 3

log "checking slurm-tracer output for job $JOB_ID"
FAIL=0
for node in "${NODES[@]}"; do
    f="out/${node}/${node}.jsonl"
    if [ ! -s "$f" ]; then
        log "FAIL: $f is empty"
        FAIL=1
        continue
    fi
    matches="$(jq -c --argjson jid "$JOB_ID" 'select(.job_id == $jid)' "$f" 2>/dev/null || true)"
    if [ -z "$matches" ]; then
        log "FAIL: no record on $node attributed to job $JOB_ID"
        log "  sample of what $node did emit:"
        tail -5 "$f" | sed 's/^/    /'
        FAIL=1
        continue
    fi
    exec_seen="$(printf '%s\n' "$matches" | jq -s 'any(.[]; .event_type == "exec")')"
    exit_seen="$(printf '%s\n' "$matches" | jq -s 'any(.[]; .event_type == "exit")')"
    if [ "$exec_seen" = "true" ] && [ "$exit_seen" = "true" ]; then
        log "PASS: $node emitted exec+exit for job $JOB_ID"
    else
        log "FAIL: $node missing exec ($exec_seen) or exit ($exit_seen) for job $JOB_ID"
        printf '%s\n' "$matches" | sed 's/^/    /'
        FAIL=1
    fi
done

log "merging per-node output into out/combined.jsonl and out/combined.csv"
per_node_files=()
for node in "${NODES[@]}"; do
    per_node_files+=("out/${node}/${node}.jsonl")
done
jq -s -c 'sort_by(.ts_ns)[]' "${per_node_files[@]}" > out/combined.jsonl

CSV_COLS='ts_ns,node,cluster,probe,cgroup_id,job_id,step_id,task_id,uid,user,account,partition,event_type,pid,tid,comm,metric,value,unit'
jq -s -r --arg cols "$CSV_COLS" '
    ($cols | split(",")) as $c
    | $c, (sort_by(.ts_ns)[] | [.[$c[]]])
    | @csv
' "${per_node_files[@]}" > out/combined.csv

if [ "$FAIL" -ne 0 ]; then
    log "FAIL: see above. Full output is under out/<node>/<node>.jsonl, merged in out/combined.{jsonl,csv}"
    exit 1
fi

log "PASS: all nodes attributed job $JOB_ID correctly"
log "combined output: out/combined.jsonl, out/combined.csv"
