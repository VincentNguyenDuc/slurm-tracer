# End-to-end test: a real Slurm cluster

Spins up a two-node Slurm 22.05 cluster in Docker, runs a job on it, and asserts
that slurm-tracer attributed that job's processes to that job's id.

```
integration-tests/e2e/run.sh          # build, run, assert, tear down on success
integration-tests/e2e/run.sh --down   # always tear down, even on failure
```

The first run takes a few minutes: it builds the image and compiles the BPF
object inside the container.

## What it checks

The unit tests cover the resolver against synthetic trees. This covers the one
thing they cannot: that the cgroup id the *kernel* stamps on an event resolves
to the job id *Slurm* actually assigned.

| Assertion | Why |
|---|---|
| Records are produced and every line is valid JSON | The stream is usable NDJSON |
| Some record carries `job_id == <the submitted job>` | The join works at all |
| The job's own binary (`st-canary`) is attributed | Not a coincidental match on some other process |
| `step_id == "batch"` | The step level of the hierarchy is parsed, not just the job |
| `uid == 2000` and `user == "alice"` | Submitter identity survives to the record |
| `exit_code == 7` reaches the record | Exit status is not lost in the exit path |
| Non-job processes stay `job_id: null` | Unattributed data is emitted, not guessed at |

## Requirements

- Docker with Compose v2 (`docker compose`, not `docker-compose`)
- A **cgroup v2** host (`stat -fc %T /sys/fs/cgroup` prints `cgroup2fs`)
- A kernel with `CONFIG_DEBUG_INFO_BTF=y`, for CO-RE

The compute-node containers run `privileged`, with `cgroup: host`, `pid: host`
and `/sys/fs/cgroup` mounted read-write. All four are needed: slurmstepd creates
cgroups, and slurm-tracer loads BPF programs and reads the kernel's BTF. This is
a throwaway test cluster, not a deployment model — the munge key is a fixed
constant baked into the image, and there is no slurmdbd.

## Why this cannot run inside the project devcontainer

The devcontainer has no Docker socket, no `CAP_SYS_ADMIN`, and mounts
`/sys/fs/cgroup` read-only, so slurmd cannot create the cgroup hierarchy at all:

```
slurmd: error: Could not create scope directory
        /sys/fs/cgroup/system.slice/<node>_slurmstepd.scope: No such file or directory
slurmd: error: cannot create cgroup context for cgroup/v2
```

Run `run.sh` from the Docker host instead.

## Layout notes worth knowing

Verified against slurmd 22.05.8 rather than taken from the documentation:

- The scope directory is `<mountpoint>/system.slice/<node>_slurmstepd.scope`.
  The node name comes **first**, so a glob anchored on `slurmstepd*.scope`
  matches nothing on a real node.
- `IgnoreSystemd=yes` changes *who* creates that directory — slurmstepd with
  `mkdir` instead of systemd over dbus — not where it is. It is set here because
  the containers have no systemd or dbus.
- Slurm places tasks it cannot number in `task_special`, which is not a task id.

`slurmd` logs `Node configuration differs from hardware` because the containers
declare `CPUs=2` while seeing the host's real core count. Harmless: Slurm only
refuses when the configured count *exceeds* the hardware.
