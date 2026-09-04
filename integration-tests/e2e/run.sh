#!/usr/bin/env bash
# End-to-end test: a real Slurm cluster, a real job, and the assertion that
# slurm-tracer attributed that job's processes to that job's id.
#
# Run from a host with Docker and a writable cgroup2 filesystem:
#     integration-tests/e2e/run.sh
#
# Leaves the cluster running on failure so it can be inspected; pass --down to
# tear it down regardless.
set -euo pipefail

cd "$(dirname "$0")"

COMPOSE=(docker compose)
NODE=c1
RECORDS=/tmp/records.ndjson
ALWAYS_DOWN=0
[ "${1:-}" = "--down" ] && ALWAYS_DOWN=1

pass=0
fail=0

ok()   { printf '  \033[32mPASS\033[0m %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  \033[31mFAIL\033[0m %s\n' "$1"; fail=$((fail + 1)); }
step() { printf '\n\033[1m==> %s\033[0m\n' "$1"; }

cleanup() {
    if [ "$fail" -eq 0 ] || [ "$ALWAYS_DOWN" -eq 1 ]; then
        step "Tearing down"
        "${COMPOSE[@]}" down -v --remove-orphans >/dev/null 2>&1 || true
    else
        printf '\nCluster left running for inspection. Tear down with:\n'
        printf '    (cd %s && docker compose down -v)\n' "$PWD"
    fi
}
trap cleanup EXIT

# --- preflight ---------------------------------------------------------------
step "Preflight"
command -v docker >/dev/null || { echo "docker not found"; exit 1; }
docker info >/dev/null 2>&1 || { echo "cannot reach the Docker daemon"; exit 1; }

# The kernel that matters is the one running the *containers*, not the one
# running this script. On macOS that is Docker Desktop's LinuxKit VM, and
# /sys/fs/cgroup does not exist on the host at all — checking it here would
# fail on exactly the setup this is meant to support.
cgroup_type=$(docker run --rm --privileged -v /sys/fs/cgroup:/sys/fs/cgroup:ro \
    debian:bookworm-slim stat -fc %T /sys/fs/cgroup 2>/dev/null | tr -d '[:space:]')
if [ "$cgroup_type" != "cgroup2fs" ]; then
    echo "the container kernel's /sys/fs/cgroup is '${cgroup_type:-unknown}', not cgroup2fs"
    echo "Slurm's cgroup/v2 plugin needs a unified hierarchy"
    exit 1
fi

# BTF is what makes the CO-RE build work; without it the BPF object cannot load.
if ! docker run --rm --privileged debian:bookworm-slim \
        test -r /sys/kernel/btf/vmlinux 2>/dev/null; then
    echo "the container kernel has no /sys/kernel/btf/vmlinux (need CONFIG_DEBUG_INFO_BTF=y)"
    exit 1
fi
echo "  docker ok, container kernel has cgroup2 and BTF"

# --- bring the cluster up ----------------------------------------------------
step "Starting the cluster (first run builds the image and the tracer)"
"${COMPOSE[@]}" up -d --build

step "Waiting for both nodes to register"
for i in $(seq 1 120); do
    idle=$("${COMPOSE[@]}" exec -T slurmctld sinfo -h -t idle -o '%D' 2>/dev/null | tr -d '[:space:]' || true)
    [ "${idle:-0}" = "2" ] && break
    sleep 2
done
"${COMPOSE[@]}" exec -T slurmctld sinfo || true
[ "${idle:-0}" = "2" ] || { bad "nodes did not reach idle"; exit 1; }
ok "2 nodes idle"

# --- start the tracer on the compute node ------------------------------------
step "Starting slurm-tracer on $NODE"
"${COMPOSE[@]}" exec -T "$NODE" rm -f "$RECORDS" /tmp/tracer.err
"${COMPOSE[@]}" exec -d "$NODE" bash -c \
    "/build/build/slurm-tracer --cluster testcluster --node $NODE \
        >$RECORDS 2>/tmp/tracer.err"

# The tracer discovers the cgroup root at startup and re-checks every 5s; give
# it a beat to attach before there is anything to see.
sleep 5
"${COMPOSE[@]}" exec -T "$NODE" cat /tmp/tracer.err || true

if "${COMPOSE[@]}" exec -T "$NODE" grep -q "attached" /tmp/tracer.err; then
    ok "tracer attached its BPF programs"
else
    bad "tracer did not attach"
    exit 1
fi

# --- run a job ---------------------------------------------------------------
step "Submitting a job as alice, pinned to $NODE"
# A distinctive comm ("st-canary") makes the assertion unambiguous: any record
# carrying it can only have come from inside this job.
JOBID=$("${COMPOSE[@]}" exec -T slurmctld runuser -u alice -- \
    sbatch --parsable -w "$NODE" -J e2e -n1 \
        --wrap 'cp /bin/true /tmp/st-canary && /tmp/st-canary && sleep 2 && exit 7')
JOBID=$(echo "$JOBID" | tr -d '[:space:]')
echo "  submitted job $JOBID"

for i in $(seq 1 60); do
    state=$("${COMPOSE[@]}" exec -T slurmctld squeue -h -j "$JOBID" -o '%T' 2>/dev/null | tr -d '[:space:]' || true)
    [ -z "$state" ] && break   # gone from the queue means finished
    sleep 1
done
echo "  job $JOBID finished"

# Exit events are emitted as the cgroup is being torn down, and the sink
# batches for up to a second; let the tail of the stream land.
sleep 5

step "Stopping the tracer"
"${COMPOSE[@]}" exec -T "$NODE" pkill -INT -x slurm-tracer || true
sleep 2
"${COMPOSE[@]}" exec -T "$NODE" cat /tmp/tracer.err | tail -5 || true

# --- assertions --------------------------------------------------------------
step "Checking the record stream"
# Copy the stream out for inspection, but run every query inside the container:
# jq is installed there, and requiring it on the host would make this fail on a
# stock macOS or a slim devcontainer.
"${COMPOSE[@]}" exec -T "$NODE" cat "$RECORDS" > ./records.ndjson
total=$(wc -l < ./records.ndjson | tr -d ' ')
echo "  captured $total records -> integration-tests/e2e/records.ndjson"

jq_count() {
    "${COMPOSE[@]}" exec -T "$NODE" \
        jq -c "select($1)" "$RECORDS" 2>/dev/null | wc -l | tr -d '[:space:]'
}
jq_show() { "${COMPOSE[@]}" exec -T "$NODE" jq -c "$1" "$RECORDS" 2>/dev/null; }

if [ "$total" -gt 0 ]; then ok "records were produced"; else bad "no records"; fi

# Every record must be valid JSON, or the stream is not NDJSON.
if "${COMPOSE[@]}" exec -T "$NODE" jq -e . "$RECORDS" >/dev/null 2>&1; then
    ok "every line is valid JSON"
else
    bad "stream contains invalid JSON"
fi

# The assertion this whole harness exists for: the kernel's cgroup id was
# resolved to the job id Slurm actually assigned.
n=$(jq_count ".job_id == $JOBID")
if [ "$n" -gt 0 ]; then
    ok "$n record(s) attributed to job $JOBID"
else
    bad "no record carried job_id $JOBID"
fi

# The canary binary only ever ran inside the job, so it must be attributed.
n=$(jq_count ".comm == \"st-canary\" and .job_id == $JOBID")
if [ "$n" -gt 0 ]; then
    ok "the job's own process (st-canary) was attributed to job $JOBID"
else
    bad "st-canary was not attributed to job $JOBID"
    jq_show 'select(.comm == "st-canary")' | head -5
fi

# sbatch work runs in the batch step.
n=$(jq_count ".job_id == $JOBID and .step_id == \"batch\"")
if [ "$n" -gt 0 ]; then
    ok "step_id 'batch' resolved"
else
    bad "no record carried step_id 'batch'"
    jq_show 'select(.job_id != null) | {job_id, step_id, task_id, comm}' | sort -u | head
fi

# The submitter's identity must survive to the record.
n=$(jq_count ".job_id == $JOBID and .user == \"alice\" and .uid == 2000")
if [ "$n" -gt 0 ]; then
    ok "uid 2000 resolved to user alice"
else
    bad "job records did not carry alice/2000"
    jq_show 'select(.job_id != null) | {uid, user}' | sort -u | head
fi

# The script exits 7; that status must reach the record rather than being lost.
n=$(jq_count ".job_id == $JOBID and .event_type == \"exit\" and .value == 7")
if [ "$n" -gt 0 ]; then
    ok "exit status 7 captured"
else
    bad "exit status 7 not found"
    jq_show 'select(.job_id != null and .event_type == "exit") | {comm, value}' | head
fi

# Processes outside any job must stay unattributed rather than being guessed at.
n=$(jq_count '.job_id == null')
if [ "$n" -gt 0 ]; then
    ok "$n non-job record(s) correctly left unattributed"
else
    echo "  note: no unattributed records seen (not a failure)"
fi

step "Sample of attributed records"
jq_show 'select(.job_id != null) | {job_id, step_id, task_id, user, event_type, comm, value}' \
    | head -12

printf '\n\033[1m%d passed, %d failed\033[0m\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
