# Docker-cluster integration test

Brings up a two-node Slurm cluster in docker compose, runs `slurm-tracer` as a
daemon on each worker, submits a job that spans both nodes, and checks that
the exec/exit events it captured are attributed to that job's id, step and
task. This is the same attribution path described in
[docs/DESIGN.md](../../docs/DESIGN.md), exercised against a real `slurmd`
instead of a synthetic cgroup tree.

## Running it

```sh
./run.sh
```

Needs Docker with a Linux kernel that has BTF
(`/sys/kernel/btf/vmlinux`) and cgroup v2 reachable from containers -- true of
a stock Docker Desktop or a native Linux Docker install. Nothing else needs
to be installed on the host; the toolchain (clang, bpftool, libbpf-dev,
Slurm, munge) lives in the image.

## What it does

`run.sh`:

1. Generates a shared munge key under `secrets/` (gitignored) if one isn't
   already there.
2. Builds `slurm-tracer` via `docker compose run --rm builder`, into
   `build/docker` on the bind-mounted repo -- not one of the `debug`/`release`
   presets, so this never collides with a developer's own Linux build tree.
   That build produces the daemon plus its probe/sink plugins as separate
   `.so` files under `build/docker/plugins/`, which is what both workers'
   configs point `plugin_dir` at.
   This has to happen in a *running* container, not during `docker compose
   build`: the BPF build reads the running kernel's BTF
   (`cmake/BpfProgram.cmake`), which an image build has no access to. See
   `scripts/build.sh`.
3. Starts one controller (`ctld`), a `collector` (a stand-in ingest endpoint
   for the http sink, `scripts/collector.py`), and two workers (`c1`, `c2`).
   Each worker runs `slurmd` and `slurm-tracer` side by side
   (`scripts/entrypoint-worker.sh`). `slurm-tracer` is given
   `--config /etc/slurm-tracer/config.json` (`conf/tracer_c1.json`/
   `conf/tracer_c2.json`, identical apart from each one's own `node` field),
   which turns on `stdout_json` (landing in
   `live/<node>/<node>.jsonl` on the host) and `http` (shipped to `collector`
   over the compose network, landing in `live/collector/received.jsonl`) at
   once -- this is as much a test of the config loader and of both sinks
   agreeing on what they were handed as it is of the probes. `live/` holds
   this raw, cumulative output for the whole run; it isn't itself the thing
   worth keeping around, see step 5.
4. Waits for both nodes to register as `idle`, then for both tracers to log
   that they found the Slurm cgroup scope (`attribution: cgroup root
   <path>, N cgroups known at startup`) -- nodes going `idle` says nothing
   about whether slurm-tracer has attached yet, and a job submitted in that
   gap comes back completely unattributed. A few seconds' settle time after
   that message is also given before submitting anything, so the cgroup
   watcher finishes attaching -- from then on it catches every job's cgroup
   directories itself, via the kernel's own `cgroup_mkdir` tracepoint, with no
   further polling.
5. Runs every scenario under `scenarios/`, in order:
   - `proc_lifecycle.sh` submits `srun --nodes=2 --ntasks-per-node=1 ...` and
     reads back the job id it printed.
   - `oom.sh` submits a 1-node, `--mem=50M` job that keeps allocating past
     that limit until the kernel's memcg OOM killer kills it -- exercises the
     `oom` probe, and depends on `conf/cgroup.conf`'s `ConstrainRAMSpace`/
     `ConstrainSwapSpace=yes` actually turning `--mem` into that job's cgroup
     `memory.max`.
   - `reconfigure.sh` rewrites c1's config to drop the `proc_lifecycle` probe
     and the `http` sink, sends `SIGHUP`, confirms the daemon logged a reload,
     then submits an OOM job to show the surviving `oom` probe/`stdout_json`
     sink still work. Restores c1's original config (and reloads it back)
     when it's done, so it must run last -- it relies on the alphabetical
     `scenarios/*.sh` ordering to sort after the other two. This is also the
     plugin lifecycle test: the two dropped components have their `.so` files
     unloaded and then loaded again from disk on the restore.

   Each scenario ends by calling `record_scenario` (`scenarios/lib.sh`),
   which snapshots its job id, the raw command output, and the *current*
   contents of every node's stdout_json output and the collector's
   http-received output (`live/`) into its own
   `out/<branch>_<commit>_<timestamp>/scenarios/<name>/` folder (`job.json`,
   `c1.jsonl`, `c2.jsonl`, `collector.jsonl`) -- self-contained, so it can be
   analyzed without cross-referencing which records in the shared
   `live/<node>/<node>.jsonl` belong to which job. Each run gets its own
   `out/<branch>_<commit>_<timestamp>/` directory so runs from different
   branches or commits don't clobber each other -- `live/` is scratch. A
   scenario's only job
   is to drive the cluster and record what happened; it makes no pass/fail
   judgment. Add a new probe's workload by dropping another script in
   `scenarios/` -- `run.sh` picks it up automatically.
6. Tears the cluster down (`docker compose down -v`), unless `--keep-up` was
   given.

## Layout

```
Dockerfile                 One image for every role (controller, worker, builder,
                           collector).
docker-compose.yml         ctld (controller) + collector + c1, c2 (workers) +
                           builder (one-shot).
conf/slurm.conf            Minimal cluster config; node names match the compose
                           services.
conf/cgroup.conf           cgroup/v2, IgnoreSystemd=yes.
conf/tracer_c1.json,       slurm-tracer's own config, one per worker (same
conf/tracer_c2.json        apart from "node"): stdout_json + http, http
                           pointed at collector, plugin_dir pointed at
                           build/docker/plugins.
scripts/build.sh           Builds slurm-tracer into build/docker.
scripts/common.sh          setup_munge, setup_cgroup_delegation, wait_for_binary.
scripts/collector.py       Stand-in http sink ingest endpoint; appends every
                           POST body to live/collector/received.jsonl.
scripts/entrypoint-*.sh    Per-role container entrypoints.
run.sh                     Brings the cluster up, runs scenarios/, tears it down.
scenarios/lib.sh           record_scenario: snapshots a scenario's job metadata
                           and current live/ node/collector output into
                           $OUT_DIR/scenarios/<name>/. Sourced, not run.
scenarios/*.sh             One workload each; drives the cluster, then calls
                           record_scenario. No pass/fail logic beyond
                           sanity-checking that the scenario's own drive step
                           worked (e.g. reconfigure.sh's reload actually
                           happened).
secrets/, live/, out/,     Generated; gitignored. live/<node> and
build/                     live/collector are the raw, cumulative bind-mount
                           targets for the whole run (docker-compose.yml);
                           out/<branch>_<commit>_<timestamp>/scenarios/<name>/
                           is the per-scenario snapshot actually worth keeping
                           -- run.sh sets OUT_DIR to that path per run.
```
