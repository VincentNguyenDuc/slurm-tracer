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
   and a `probe.cpp` implementing `Probe` (`core/probe/probe.h`). Every event begins
   with `struct st_event_hdr` (see [src/core/events.h](src/core/events.h)),
   which carries the cgroup id used for job attribution — the probe's own event
   struct and `type` values stay in its directory, not shared.
2. Give it its own `CMakeLists.txt` (`add_st_probe(<name> SOURCES probe.cpp BPF
   <name>.bpf.c)`) and one `add_subdirectory(probes/<name>)` line in
   [src/CMakeLists.txt](src/CMakeLists.txt).
3. Register it from `probe.cpp` by exporting
   `extern "C" void st_register_probe(Registry<Probe>&)`, the one symbol every
   probe plugin exports. The build turns the directory into
   `plugins/probes/libst_probe_<name>.so`, which the daemon `dlopen()s` the
   first time a config names it — so nothing in core ever names a probe, and
   nothing links one in.

No other file changes — that's the plugin contract, detailed further in
[docs/DESIGN.md](docs/DESIGN.md). A config entry names an *instance*, not a
plugin: the key is its id and an optional `"plugin"` setting picks the `.so`
(defaulting to the id), so one plugin can run several times with different
settings and each instance is identified — in logs, in a reload diff, and in
a record's `probe` field — by its own id. `proc_lifecycle` and `oom` are the
event-driven telemetry probes that exist today. The cgroup watcher is not one
of them: attribution is mandatory, not a metric a cluster can opt out of, so
it is built and wired directly rather than through this plugin contract.
Every probe here streams individual events — there is no in-kernel
aggregating shape.
