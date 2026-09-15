# slurm-tracer

eBPF-based observability for Slurm compute nodes.

Slurm's own accounting samples cgroup counters every 30 seconds and tells you what a
job *consumed*. `slurm-tracer` uses eBPF to capture what a job actually *did* — process
lifecycle, scheduling delay, I/O and memory behaviour — attributed to the exact
job/step/task, and streams it somewhere useful.

## Layout

```
src/core/            record model, attribution, sink interface. No libbpf, so
                     tests exercise it without CAP_BPF.
src/core/events.h    wire format shared by BPF and userspace (st_event_hdr)
src/probes/          one directory per probe: its .bpf.c, its event struct,
                     its userspace class
src/sinks/           one directory per output
cmake/BpfProgram.cmake   compiles *.bpf.c and generates libbpf skeletons
cmake/StPlugin.cmake     add_st_probe()/add_st_sink(), generates the registry
integrations/        end-to-end tests against a real Slurm cluster
docs/DESIGN.md       architecture, attribution model, roadmap
```

**Core never names a plugin.** `src/core` must contain no probe or sink identifier
and no `#include` of anything under `src/probes/` or `src/sinks/` — that rule, not
the directory split, is what keeps core testable without CAP_BPF or a kernel.

## Adding a probe

1. Create `src/probes/<name>/` holding `<name>.bpf.c`, `<name>_events.h`,
   and a `probe.cpp` implementing `Probe` (`core/probe.h`). Every event begins
   with `struct st_event_hdr` (see [src/core/events.h](src/core/events.h)),
   which carries the cgroup id used for job attribution — the probe's own event
   struct and `type` values stay in its directory, not shared.
2. Give it its own `CMakeLists.txt` (`add_st_probe(<name> SOURCES probe.cpp BPF
   <name>.bpf.c)`) and one `add_subdirectory(probes/<name>)` line in
   [src/CMakeLists.txt](src/CMakeLists.txt).
3. Register it from `probe.cpp` with `register_<name>(Registries&)`; the build
   generates the manifest that calls it, so nothing in core ever names a probe.

No other file changes — that's the plugin contract in
[docs/DESIGN.md](docs/DESIGN.md) §5. `sched_latency` (aggregate) and
`proc_lifecycle` (event-driven) are the two probes that exist today, and
between them cover both shapes in §6.
