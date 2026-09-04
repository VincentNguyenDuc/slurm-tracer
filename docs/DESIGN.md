# slurm-tracer — Design

## 1. Why

Slurm already accounts jobs. `jobacct_gather/cgroup` samples cgroup counters on
`JobAcctGatherFrequency` (30 s by default) and `sacct` reports peak RSS, total CPU
time, and exit codes. That answers *what a job consumed*. It does not answer:

- **Why was the job slow?** Run-queue latency, I/O stalls, and page-cache misses are
  invisible to interval sampling of counters.
- **Who hurt whom?** On a shared node, nothing attributes contention to the
  neighbouring job that caused it.
- **What did the job actually do?** No process tree, no I/O targets, no endpoints.
- **What happened in the last 5 seconds before the OOM?** Anything shorter than the
  sampling interval never existed.

eBPF closes exactly this gap: event-level detail, aggregated in-kernel so the volume
stays sane, attributed to a specific job/step, at a cost sampling can't reach.

## 2. Goals and non-goals

**Goals**

- Every datum carries job/step/task attribution.
- Probes are independent modules; adding one touches no core file.
- Probes toggle on and off per node without a rebuild.
- One daemon per compute node, bounded overhead, degrades gracefully on
  heterogeneous kernels.
- Output is not bound to a backend. Multiple sinks run concurrently.
- Data lands in a warehouse under a schema that survives new probes.

## 3. Architecture

```
kernel
 ┌───────────────────────────────────────────────────────┐
 │ probe modules (BPF CO-RE objects)                     │
 │   proc_lifecycle   sched_latency   bio   tcp   oom    │
 │        │ discrete events        │ aggregates          │
 │        ▼                        ▼                     │
 │   BPF_MAP_TYPE_RINGBUF     per-cgroup HASH / histogram│
 └────────┬────────────────────────┬─────────────────────┘
          │ epoll                  │ read on interval
──────────┼────────────────────────┼──────────────────────
userspace │                        │
 ┌────────▼────────────────────────▼─────────────────────┐
 │ collector core                                        │
 │   registry ....... probe lifecycle, failure isolation │
 │   attribution .... cgroup_id → job / step / task / uid│
 │   enrichment ..... cluster, node, partition, account  │
 │   batching                                            │
 └────────────────────────┬──────────────────────────────┘
                          │ batches of st::Record
 ┌────────────────────────▼──────────────────────────────┐
 │ sink fan-out (each sink: bounded queue + worker)      │
 │   stdout_json  │  http  │  prometheus  │  file        │
 └────────────────────────┬──────────────────────────────┘
                          ▼
        gateway → object store (Parquet) → warehouse → dashboard
                          └── or watch the JSON stream live
```

## 4. Attribution — cgroup id to Slurm job

This is the load-bearing idea of the whole design.

In the kernel, `bpf_get_current_cgroup_id()` returns the current task's cgroup v2 id,
which is the inode number of the cgroup directory. It is a field read — no locks, no
task-struct walk, safe in any context. Every probe stamps it on every record. That is
the *only* attribution work done in kernel space.

The helper only answers for *the running task*, though, which is not always the
record's subject: a scheduler hook fires on the waker, not the woken, and on the
outgoing task, not the incoming one. A probe in that position reads the same cgroup
id straight off the task it actually cares about instead of the helper. Either way
the kernel side hands userspace one opaque id and nothing else — which task it came
from is the probe's problem, not the resolver's.

Userspace turns that id into a job. Slurm's cgroup/v2 hierarchy (22.05+, systemd
integration) looks like:

```
/sys/fs/cgroup/system.slice/<node>_slurmstepd.scope/
  system/                          ← slurmstepd itself, not job work
  job_<jobid>/
    step_<stepid>/                 ← stepid ∈ {0..N, batch, extern, interactive}
      user/
        task_<taskid>/             ← one per rank
        task_special/              ← tasks Slurm cannot number
      slurm/
```

**Resolver**

1. At startup, walk the discovered cgroup root. For each directory, `stat()` it — the
   inode number *is* the cgroup id — and parse job/step/task out of the path. Populate
   a flat hash map.
2. Watch the root with `inotify` (`IN_CREATE`/`IN_DELETE` on directories) so new steps
   land in the map without rescanning.
3. On a cache miss — an event can beat the inotify notification — do a targeted
   rescan. If it is still unknown, emit the record with a null `job_id` rather than
   dropping it. Unattributed data is worth more than no data, and a rising
   unattributed rate is itself the signal that the resolver is misconfigured.
4. Retain entries for a grace period after the cgroup disappears: exit events arrive
   while the cgroup is already being torn down.
5. Guard against inode reuse — a recycled cgroup id must not inherit the previous
   job's identity. Entries carry a creation timestamp; a record older than the entry
   is treated as a miss.

**Why not resolve in the kernel?** We could walk `task->cgroups` and parse the path in
BPF, or use `bpf_get_current_ancestor_cgroup_id()`. Both push string parsing into a
verifier-constrained hot path and hard-code Slurm's layout into a program that must
pass the verifier on every kernel we support. Resolving in userspace makes a Slurm
version change a config edit instead of a reverification.

**Beyond the path.** `job_id` alone is thin — dashboards want user, account,
partition, QOS. Resolve those once per job, asynchronously, off the hot path (from the
step's `SLURM_*` environment, or one cached `scontrol show job`), and attach them at
the sink. Never in the poll loop.

## 5. Probe module contract

A probe contributes one `.bpf.c`, one C++ class, and event or map-value structs
private to its own directory — the core never includes them, and demuxes a ring
buffer knowing only that every event starts with `st_event_hdr`.

```cpp
class Probe {
public:
    virtual ~Probe() = default;
    virtual std::string_view name() const = 0;      // "proc_lifecycle"

    virtual bool open(const ComponentConfig&) = 0;  // skeleton open, set rodata knobs
    virtual bool load()  = 0;                       // the verifier runs here
    virtual bool attach() = 0;
    virtual void detach() = 0;

    // Event-driven probes hand the core a ring buffer to poll.
    virtual int  ring_fd() const { return -1; }
    virtual void on_event(const void* data, size_t len, RecordEmitter&) = 0;

    // Aggregating probes leave on_event() empty; the core calls this on the
    // same cadence records are flushed to sinks instead. An aggregate is only
    // ever as fresh as the next flush — that's the shape, not a bug.
    virtual void poll(RecordEmitter&) {}
};
```

A probe registers itself by name from its own `.cpp` (`r.probes.add("name", ...)`,
called from a generated manifest that names every plugin the build contains) — so
adding a probe is two new files, one `add_st_probe()` line in the probe's own
`CMakeLists.txt`, and no edits to core. See [registry.h](../src/core/registry.h)
for why this is a generated manifest rather than a static initialiser.

**Failure isolation is a requirement, not a nicety.** A probe that fails to load —
missing tracepoint, verifier rejection, kernel too old — is disabled with a warning
and the daemon keeps running with the rest. Clusters are heterogeneous; a node with an
older kernel should lose one probe, not all observability.

**One ring buffer per probe**, not a shared one. It costs a few MiB, and in exchange a
chatty probe cannot starve a quiet one, and each buffer is sized to its own event rate.

## 6. Events versus aggregates

Two data shapes, and picking wrong is how eBPF tooling destroys a cluster.

| | Events (ringbuf) | Aggregates (per-cgroup map) |
|---|---|---|
| For | rare, individually interesting | high-frequency, interesting only statistically |
| Examples | exec, exit, OOM kill, job failure | run-queue latency, bio latency, bytes moved |
| Kernel rate | 10–10³/s | 10⁵–10⁷/s |
| Userspace cost | one record per event | one map read per cgroup per interval |

**Rule: if it can exceed ~10⁴/s per node, aggregate in the kernel.** Never stream
per-syscall or per-packet events. Latency distributions go in log₂-bucketed histogram
maps keyed by cgroup id, flushed on the poll interval — `sched_latency` (run-queue
wait time) is the first probe built this way; `bio`, `tcp` and `oom` are the same
shape, not yet built.

## 7. Configuration

`/etc/slurm-tracer/config.toml`:

```toml
[node]
cluster       = "prod"
poll_interval = "10s"

[slurm]
# Auto-discovered when omitted; override for IgnoreSystemd or a custom mountpoint.
cgroup_root = "/sys/fs/cgroup/system.slice/slurmstepd.scope"

[probes.proc_lifecycle]
enabled = true

[probes.sched_latency]
enabled = true
buckets = 20

[probes.bio]
enabled = false

[[sinks]]
type = "stdout_json"

[[sinks]]
type     = "http"
endpoint = "https://ingest.internal/v1/telemetry"
batch    = 1000
flush    = "5s"
```

`SIGHUP` reloads: probes attach and detach in place without restarting the daemon,
which keeps the attribution cache warm and avoids a gap in coverage.

## 8. Sinks

```cpp
class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(std::vector<Record> batch) = 0;  // always batched
    virtual void flush() = 0;                            // drain queued batches
    virtual uint64_t dropped() const = 0;                // overflow, not errors
};
```

The core fans every batch out to all configured sinks. **A sink must never block the
poll loop.** Each owns a bounded queue and a worker thread; on overflow it drops the
oldest batch and increments a counter — which is itself exported, so drops are visible
rather than silent. Backpressure must not reach the ring buffer: a stalled HTTP
endpoint cannot be allowed to cost us kernel events.

First two sinks are `stdout_json` (trivially debuggable, pipes into any existing log
shipper) and `http` (NDJSON batches). Prometheus and OTLP arrive later behind the same
interface — which is the entire reason the interface exists now rather than later.

## 9. Record model and the warehouse

**One wide record type, not one table per probe.** Adding a probe must never require a
schema migration.

```
ts_ns, node, cluster, probe                       ── provenance
job_id, step_id, task_id, uid, user,
        account, partition                        ── attribution
event_type, pid, tid, comm                        ── identity
metric, value (f64), unit                         ── numeric payload
attrs: map<string,string>                         ── probe-specific detail
```

In the warehouse this lands as a single fact table partitioned by
`(date, cluster, node)`, in Parquet/Iceberg. A new probe adds new `metric` values and
new `attrs` keys — never new columns. Dashboards filter on `probe`/`metric`. The cost
is some storage bloat versus typed columns; schema stability across a growing probe
set is worth it.

**Cardinality discipline:** `attrs` may hold unbounded values (paths, addresses, pids)
because a warehouse handles high cardinality fine. Those keys must never be promoted
into Prometheus-style *labels*. Keep the two paths distinct or the metrics backend
will fall over.

`job_id` + `cluster` is the join key back to Slurm's accounting database, so a
dashboard can put eBPF detail directly beside the official `sacct` record. That join
is the point of the whole exercise.
