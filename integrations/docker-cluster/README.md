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
3. Starts one controller (`ctld`) and two workers (`c1`, `c2`). Each worker
   runs `slurmd` and `slurm-tracer` side by side (`scripts/entrypoint-worker.sh`),
   with slurm-tracer's stdout_json output landing in `out/<node>/<node>.jsonl`
   on the host.
4. Waits for both nodes to register as `idle`, then for both tracers to log
   that they found the Slurm cgroup scope (`attribution: cgroup root
   appeared...`) -- see "Timing" below for why that's a separate wait.
5. Submits `srun --nodes=2 --ntasks-per-node=1 ...` and reads back the job id
   it printed.
6. Checks `out/c1/c1.jsonl` and `out/c2/c2.jsonl` for records with that
   `job_id`, and that both an `exec` and an `exit` event were captured.
7. Merges the per-node output into `out/combined.jsonl` (all records,
   time-sorted) and `out/combined.csv` (same, flattened to the `Record`
   fields in `src/core/record.h` -- `attrs` excluded since it's a map).
   Written whether the checks passed or not, so a failure still leaves
   something to inspect.
8. Tears the cluster down (`docker compose down -v`), unless `--keep-up` was
   given.


## Layout

```
Dockerfile                 One image for every role (controller, worker, builder).
docker-compose.yml         ctld (controller) + c1, c2 (workers) + builder (one-shot).
conf/slurm.conf            Minimal cluster config; node names match the compose services.
conf/cgroup.conf           cgroup/v2, IgnoreSystemd=yes.
scripts/build.sh           Builds slurm-tracer into build/docker.
scripts/common.sh          setup_munge, setup_cgroup_delegation, wait_for_binary.
scripts/entrypoint-*.sh    Per-role container entrypoints.
test.sh                    Orchestrates the above and checks the result.
secrets/, out/, build/     Generated; gitignored.
```
