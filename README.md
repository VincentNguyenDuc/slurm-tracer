# slurm-tracer

eBPF-based observability for Slurm compute nodes.

Slurm's own accounting samples cgroup counters every 30 seconds and tells you what a
job *consumed*. `slurm-tracer` uses eBPF to capture what a job actually *did* — process
lifecycle, scheduling delay, I/O and memory behaviour — attributed to the exact
job/step/task, and streams it somewhere useful.

**Status: M0.** The build pipeline and one probe (`proc_lifecycle`) work end to end.
Job attribution, the probe registry, configuration, and sinks are designed but not yet
implemented. See [docs/DESIGN.md](docs/DESIGN.md).

## Requirements

- Linux ≥ 5.8 with `CONFIG_DEBUG_INFO_BTF=y` (check: `ls /sys/kernel/btf/vmlinux`)
- clang, llvm, bpftool, `libbpf-dev` ≥ 1.0
- CMake ≥ 3.16, Ninja, and vcpkg for `fmt` / `spdlog` / `Catch2`
- `CAP_BPF` + `CAP_PERFMON` (or root) to load probes

The devcontainer in [.devcontainer/](.devcontainer/) provides all of this and requests
the capabilities needed to actually load BPF programs.

## Quickstart

```bash
make build                 # BUILD_PRESET=debug|release|profile
sudo ./build/debug/slurm-tracer
```

`make vmlinux` dumps the running kernel's BTF if you want it outside the build.

## Layout

```
bpf/                 BPF CO-RE programs, one per probe (*.bpf.c)
include/events.h     wire format shared by BPF and userspace
src/                 collector daemon
cmake/BpfProgram.cmake   compiles *.bpf.c and generates libbpf skeletons
docs/DESIGN.md       architecture, attribution model, roadmap
```

## Adding a probe

1. Write `bpf/<name>.bpf.c`. Every event begins with `struct st_event_hdr` (see
   [include/events.h](include/events.h)), which carries the cgroup id used for job
   attribution.
2. Register it in [CMakeLists.txt](CMakeLists.txt):

   ```cmake
   add_bpf_program(<name> SOURCE bpf/<name>.bpf.c INCLUDES include)
   ```

3. Link `bpf::<name>` and `#include "<name>.skel.h"`.

The BPF ISA level defaults to `-mcpu=v3` (kernel ≥ 5.1); lower it with
`-DBPF_CPU=v2` for older compute nodes. To build against a kernel other than the build
host's, point `-DBPF_VMLINUX_BTF=` at a vendored BTF blob.
