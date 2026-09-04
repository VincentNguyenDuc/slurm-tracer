# slurm-tracer

eBPF-based observability for Slurm compute nodes.

Slurm's own accounting samples cgroup counters every 30 seconds and tells you what a
job *consumed*. `slurm-tracer` uses eBPF to capture what a job actually *did* — process
lifecycle, scheduling delay, I/O and memory behaviour — attributed to the exact
job/step/task, and streams it somewhere useful.

## Layout

Core is `src/core` and `src/runtime`; everything optional is a plugin under
`src/plugins`. Headers sit next to their sources, and `src/` is the only include
root — so every include is layer-qualified, e.g. `#include "core/record.h"`.

```
src/core/            record model, attribution, sink interface. No libbpf, so
                     tests exercise it without CAP_BPF.
src/core/events.h    wire format shared by BPF and userspace (st_event_hdr)
src/runtime/         libbpf: skeleton lifecycle, ring buffer poll loop
src/plugins/probes/  one directory per probe: its .bpf.c, its event struct,
                     its userspace class
src/plugins/sinks/   one directory per output
cmake/BpfProgram.cmake   compiles *.bpf.c and generates libbpf skeletons
docs/DESIGN.md       architecture, attribution model, roadmap
```

**Core never names a plugin.** `src/core` and `src/runtime` must contain no probe
or sink identifier and no `#include "plugins/..."` — that rule, not the directory
nesting, is what keeps the split real.

## Adding a probe

1. Create `src/plugins/probes/<name>/` holding `<name>.bpf.c` and `<name>_events.h`.
   Every event begins with `struct st_event_hdr` (see
   [src/core/events.h](src/core/events.h)), which carries the cgroup id used for job
   attribution. The probe's own event struct and its `type` values stay in its
   directory — they are not shared.
2. Register it in [CMakeLists.txt](CMakeLists.txt):

   ```cmake
   add_bpf_program(<name> SOURCE src/plugins/probes/<name>/<name>.bpf.c INCLUDES src)
   ```

3. Link `bpf::<name>` and `#include "<name>.skel.h"`.

> Being reworked: probes and sinks are becoming registry-driven plugins, after
> which step 2 becomes an `add_st_probe()` call in the probe's own
> `CMakeLists.txt`. See [docs/DESIGN.md](docs/DESIGN.md) §5.
