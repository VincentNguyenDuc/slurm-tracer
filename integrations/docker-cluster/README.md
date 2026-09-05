# Docker-cluster integration test

Brings up a two-node Slurm cluster in docker compose, runs `slurm-tracer` as a
daemon on each worker, submits a job that spans both nodes, and checks that
the exec/exit events it captured are attributed to that job's id, step and
task. This is the same attribution path described in
[docs/DESIGN.md §4](../../docs/DESIGN.md), exercised against a real `slurmd`
instead of a synthetic cgroup tree.

## Running it

```sh
./test.sh            # build, run, check, tear down
./test.sh --keep-up   # leave the cluster running after a failure, for debugging
```

Needs Docker with a Linux kernel that has BTF
(`/sys/kernel/btf/vmlinux`) and cgroup v2 reachable from containers -- true of
a stock Docker Desktop or a native Linux Docker install. Nothing else needs
to be installed on the host; the toolchain (clang, bpftool, libbpf-dev,
Slurm, munge) lives in the image.

## What it does

1. Generates a shared munge key under `secrets/` (gitignored) if one isn't
   already there.
2. Builds `slurm-tracer` via `docker compose run --rm builder`, into
   `build/docker` on the bind-mounted repo -- not one of the `debug`/`release`
   presets, so this never collides with a developer's own Linux build tree.
   This has to happen in a *running* container, not during `docker compose
   build`: the BPF build reads the running kernel's BTF
   (`cmake/BpfProgram.cmake`), which an image build has no access to. See
   `scripts/build.sh`.
3. Starts one controller (`ctld`), a `collector` (a stand-in ingest endpoint
   for the http sink, `scripts/collector.py`), and two workers (`c1`, `c2`).
   Each worker runs `slurmd` and `slurm-tracer` side by side
   (`scripts/entrypoint-worker.sh`). `slurm-tracer` is given
   `--config /etc/slurm-tracer/config.toml` (`conf/tracer.toml`, identical on
   both workers), which turns on `stdout_json` (landing in
   `out/<node>/<node>.jsonl` on the host) and `http` (shipped to `collector`
   over the compose network, landing in `out/collector/received.jsonl`) at
   once -- this is as much a test of the config loader and of both sinks
   agreeing on what they were handed as it is of the probes.
4. Waits for both nodes to register as `idle`, then for both tracers to log
   that they found the Slurm cgroup scope (`attribution: cgroup root
   appeared...`) -- nodes going `idle` says nothing about whether slurm-tracer
   has attached yet, and a job submitted in that gap comes back completely
   unattributed (`src/daemon.cpp`'s `kDiscoveryRetry` is 5s). A few seconds'
   settle time after that message is also given before submitting anything,
   since the resolver still needs to notice the job's own cgroup directories
   as they're created.
5. Submits `srun --nodes=2 --ntasks-per-node=1 ...` and reads back the job id
   it printed.
6. Checks `out/c1/c1.jsonl` and `out/c2/c2.jsonl` for records with that
   `job_id`, and that both an `exec` and an `exit` event were captured
   (`proc_lifecycle`).
7. Submits a second, deliberately oversubscribed workload -- 32 tasks per
   node against a node configured with 1 CPU -- to give the aggregating
   `sched_latency` probe (docs/DESIGN.md §6) real run-queue contention to
   bucket, and checks that its histogram records show up attributed to that
   job. Prints the merged per-node histogram so a human can see the actual
   latency distribution, not just pass/fail.
8. Checks `out/collector/received.jsonl` for the same two jobs -- proof that
   the http sink actually delivered over the network, from both workers, not
   just that it ran.
9. Merges the per-node output into `out/combined.jsonl` (all records,
   time-sorted) and `out/combined.csv` (same, flattened to the `Record`
   fields in `src/core/record.h` -- `attrs` excluded since it's a map).
   Written whether the checks passed or not, so a failure still leaves
   something to inspect. (The collector's file is left out of this merge --
   it is the same records a second time, over http instead of stdout_json,
   not new data.)
10. Tears the cluster down (`docker compose down -v`), unless `--keep-up` was
    given.


## Layout

```
Dockerfile                 One image for every role (controller, worker, builder,
                           collector).
docker-compose.yml         ctld (controller) + collector + c1, c2 (workers) +
                           builder (one-shot).
conf/slurm.conf            Minimal cluster config; node names match the compose
                           services. debug partition is OverSubscribe=FORCE:32
                           so the sched_latency workload can put more tasks on
                           a node than it has CPUs -- plain `srun --overcommit`
                           can't do that for a fresh allocation, only for a
                           step inside one already sized normally, which is
                           why test.sh's job 2 wraps it in a salloc.
conf/cgroup.conf           cgroup/v2, IgnoreSystemd=yes.
conf/tracer.toml           slurm-tracer's own config (docs/DESIGN.md §7):
                           stdout_json + http, http pointed at collector.
scripts/build.sh           Builds slurm-tracer into build/docker.
scripts/common.sh          setup_munge, setup_cgroup_delegation, wait_for_binary.
scripts/collector.py       Stand-in http sink ingest endpoint; appends every
                           POST body to out/collector/received.jsonl.
scripts/entrypoint-*.sh    Per-role container entrypoints.
test.sh                    Orchestrates the above and checks the result.
secrets/, out/, build/     Generated; gitignored.
```
