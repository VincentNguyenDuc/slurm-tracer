#!/bin/bash
# Brings up a two-node Slurm cluster in docker compose, runs slurm-tracer as a
# daemon on each worker, runs every scenario in scenarios/ against it, and
# tears the cluster back down -- leaving their raw output under
# out/<branch>_<commit>_<timestamp>/, so runs from different branches or
# commits don't clobber each other and stay easy to tell apart.
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
        echo "run.sh: --keep-up set, leaving the cluster running (docker compose down when done)"
        return
    fi
    echo "run.sh: tearing down"
    docker compose down -v --remove-orphans >/dev/null 2>&1 || true
    exit $status
}
trap cleanup EXIT

log() { echo "run.sh: $*"; }

# Branch names can contain "/" (e.g. feature/foo), which isn't safe as a
# path component -- flatten it. Falls back to "detached"/"unknown" outside a
# normal branch checkout or git repo (e.g. CI checking out a bare commit).
BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null | tr '/' '-')"
COMMIT="$(git rev-parse --short HEAD 2>/dev/null)"
export OUT_DIR="out/${BRANCH:-detached}_${COMMIT:-unknown}_$(date +%Y%m%d_%H%M%S)"

log "resetting live/, writing this run's output to $OUT_DIR"
mkdir -p secrets live/c1 live/c2 live/collector "$OUT_DIR/scenarios"
if [ ! -s secrets/munge.key ]; then
    log "generating munge key"
    head -c 1024 /dev/urandom > secrets/munge.key
fi
# live/<node>/<node>.jsonl and live/collector/received.jsonl are the raw,
# cumulative sources scenarios/*.sh snapshot from (see scenarios/lib.sh) --
# truncate them once here so a fresh run doesn't carry over a previous run's
# records. out/ ends up holding only scenarios/, the thing worth keeping.
: > live/c1/c1.jsonl
: > live/c2/c2.jsonl
: > live/collector/received.jsonl

log "building the cluster image"
# --progress plain: the interactive (tty) build renderer calls ConsoleFromFile
# on stdout, which fails with "failed to get console: provided file is not a
# console" as soon as stdout is redirected (as it is here) while stderr is
# still a terminal -- i.e. every interactive run. Plain progress has no such
# dependency. BUILDKIT_PROGRESS=plain is not honoured by compose v2 here.
docker compose --progress plain build >/dev/null

log "building slurm-tracer (needs the running kernel's BTF; see scripts/build.sh)"
docker compose run --rm builder

log "starting controller, collector and workers"
docker compose up -d ctld collector c1 c2

wait_for_idle() {
    local tries=60
    for i in $(seq 1 "$tries"); do
        for svc in ctld collector c1 c2; do
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

# Wait for each tracer to found cgroup, or a job submitted too early
# comes back with every record unattributed.
wait_for_attribution() {
    local tries=30
    for i in $(seq 1 "$tries"); do
        local ready=1
        for node in "${NODES[@]}"; do
            # Matches daemon.cpp's wordings: "cgroup root <path>, N cgroups known at startup".
            docker compose exec -T "$node" grep -q 'attribution: cgroup root' "/var/log/slurm-tracer/${node}.log" 2>/dev/null || ready=0
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
# The log line above only means the resolver bootstrapped against whatever
# cgroups already existed; give the cgroup_lifecycle probe (which is what
# actually catches job_/step_/task_ directories from here on, via the kernel's
# own cgroup_mkdir tracepoint -- see src/probes/cgroup_lifecycle) a moment to
# finish attaching before anything is submitted.
sleep 3

log "running scenarios"
for scenario in scenarios/*.sh; do
    # scenarios/lib.sh is a shared helper sourced by the actual scenarios, not
    # a scenario itself -- it's also not executable, so running it here would
    # fail.
    [ "$(basename "$scenario")" = "lib.sh" ] && continue
    log "-- $scenario"
    "$scenario"
done

log "done; output under $OUT_DIR"
