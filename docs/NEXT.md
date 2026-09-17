# Hot-reload of probes and sinks via SIGHUP

## Context

`docs/DESIGN.md` §7 has named this as a roadmap item since early on: *"Reload on
SIGHUP — so probes can attach and detach without restarting the daemon and losing
the warm attribution cache — is still roadmap, not built."* The motivating case: an
operator adds or removes a probe/sink in the live config file while `slurm-tracer` is
running against a real Slurm cluster, and wants that to take effect without a
restart — a restart means losing `CgroupResolver`'s warm cgroup-id → job map and a
window where in-flight jobs go unattributed.

Two decisions were made while scoping this:

- **CLI flags are being removed entirely** except `--config <path>`. Every setting
  (`cluster`, `node`, `cgroup_root`, `batch_size`, `flush_interval`, `verbose`,
  `probes`, `sinks`) already has a home in the JSON config format (confirmed by
  reading `config_file.cpp`'s parser), so this strands nothing — it just removes the
  question of how a CLI override should interact with a later reload, by deleting
  the only other configuration path.
- **Converting probes/sinks into `dlopen`-loaded shared objects** (Slurm's own
  plugin model: `AuthType=auth/munge` → `auth_munge.so`, resolved and loaded at
  runtime) is an explicit *future* direction, not part of this plan. This plan keeps
  probes/sinks statically linked (as they are today) and only makes the *set* of
  already-linked-in ones reloadable by name. The design below is deliberately
  shaped so that follow-up isn't blocked (see "Shaping for the future `.so` plugin
  model" at the end) — but no dlopen/ABI work happens now.

## Design

### `Config` gains a remembered file path

`src/core/config/config.h` — add one field:

```cpp
std::string config_path; // empty = this Config has no file to reload from
```

`src/core/config/config_file.cpp` — inside `load_config_file`, stamp it right after
`Config config;` is declared: `config.config_path = path;`. This is the one place
that knows the path, so it's the one place responsible for remembering it — no
change needed in `main.cpp` beyond what §1 already does.

### Config diffing — new pure-logic module

`ComponentConfig` has no `operator==` today and no way to enumerate its internal
`values_` map from outside. Add to `src/core/config/config.h`/`.cpp`:

```cpp
bool operator==(const ComponentConfig& other) const;             // config.cpp: values_ == other.values_
bool operator!=(const ComponentConfig& other) const { return !(*this == other); }
```

New `src/core/config/config_diff.h` / `.cpp` (added to `st_core`'s source list in
`src/core/CMakeLists.txt`, so it's libbpf-free and directly unit-testable, unlike
`daemon.cpp`/`event_loop.cpp` which only ever compile into the `slurm-tracer`
executable target):

```cpp
struct ConfigDiff {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> changed; // same name, ComponentConfig differs
    bool empty() const { return added.empty() && removed.empty() && changed.empty(); }
};

// `current`/`next` must both already be resolved to their *effective* form
// (Daemon's `selected()`), never the raw Config::probes/sinks map -- diffing the
// raw maps would misread "the file's probes section was removed" (which actually
// means "fall back to every registered probe") as "everything got removed".
ConfigDiff diff_components(
    const std::map<std::string, ComponentConfig>& current,
    const std::map<std::string, ComponentConfig>& next
);
```

Implementation is a straightforward sorted-map merge-diff (both are `std::map`, so
already key-ordered) — one pass, three buckets.

### `EventLoop::remove()` — the only way to drop one probe's ring buffer

Confirmed hazard: `ring_buffer__add`/`__new` take a raw fd and never `dup()` it.
`BpfProbe<Skel>::detach()` closes that fd via the skeleton's generated `__destroy`.
Closing it while still registered in a *live, currently-polled* `ring_buffer*` is a
use-after-close hazard (and can collapse the kernel map itself, since the fd close
may drop its last reference). So removal must free the whole `ring_buffer*` and
rebuild it from the surviving fd set — **and this must complete before that probe's
own `detach()` runs**, not after.

```cpp
// src/core/event_loop.h
void remove(Probe& probe);
```

```cpp
// src/core/event_loop.cpp
void EventLoop::remove(Probe& probe) {
    const auto it = std::find_if(bindings_.begin(), bindings_.end(),
        [&](const std::unique_ptr<Binding>& b) { return b->probe == &probe; });
    if (it == bindings_.end()) return;
    bindings_.erase(it); // Binding is heap-allocated; erase doesn't move it or
                          // any other Binding's address, which is what libbpf's
                          // ctx pointers below are keyed on.

    if (rb_ != nullptr) { ring_buffer__free(rb_); rb_ = nullptr; }
    if (bindings_.empty()) return; // matches the existing empty() contract

    ring_buffer_sample_fn callback = [](void* raw, void* data, size_t size) -> int {
        auto& b = *static_cast<Binding*>(raw);
        b.probe->on_event(data, size, *b.out);
        return 0;
    };
    for (auto& binding : bindings_) {
        const int fd = binding->probe->ring_fd(); // re-derived fresh each call
        const bool ok = rb_ == nullptr
            ? (rb_ = ring_buffer__new(fd, callback, binding.get(), nullptr)) != nullptr
            : ring_buffer__add(rb_, fd, callback, binding.get()) == 0;
        if (!ok)
            spdlog::error("probe {}: failed to re-arm while removing {}", binding->probe->name(), probe.name());
    }
}
```

No hazard from bindings reordering: libbpf dispatches by fd/ctx, not position, and
`ring_fd()` is re-derived fresh from `bpf_map__fd(skel()->maps.events)` each call
(verified against `src/probes/proc_lifecycle/probe.cpp`).

*Adding* a probe needs no new `EventLoop` capability — `ring_buffer__add` already
works against an already-being-polled `ring_buffer*`; it isn't a startup-only call.

### `Pipeline::set_sinks()` — the only way to change the sink list

`src/core/pipeline/pipeline.h`, inlined next to the existing `set_resolver`:

```cpp
void set_sinks(std::vector<Sink*> sinks) { sinks_ = std::move(sinks); }
```

Safe because `ship()` always runs to completion synchronously, and reload only ever
runs between `Daemon::run()`'s loop iterations — never from inside `emit()`,
`tick()`, `flush()`, or a probe callback. `batch_` has no per-sink assumptions
baked in, so a sink added mid-reload simply starts receiving future batches, and one
removed simply stops.

### `Daemon` — shared add/remove helpers, used by both startup and reload

`src/core/daemon.h` — signature and member changes:

```cpp
int run(const volatile std::sig_atomic_t& stop, volatile std::sig_atomic_t& reload); // reload: non-const, cleared after each handled reload

private:
    void start_cgroup_watcher();
    void report_shutdown() const;

    void reload_config();                                                       // NEW
    void add_probe(const std::string& name, const ComponentConfig& config);      // NEW
    void remove_probe(const std::string& name);                                  // NEW
    void add_sink(const std::string& name, const ComponentConfig& config);       // NEW
    void remove_sink(const std::string& name);                                   // NEW
    void rewire_pipeline_sinks();                                                // NEW
```

`src/core/daemon.cpp`:

```cpp
void Daemon::add_sink(const std::string& name, const ComponentConfig& config) {
    auto sink = registries_.sinks.create(name, config);
    if (!sink) { spdlog::warn("sink {}: not in this build, skipped", name); return; }
    sinks_.push_back(std::move(sink));
    if (pipeline_) rewire_pipeline_sinks(); // null only during start(), before Pipeline exists
}

void Daemon::add_probe(const std::string& name, const ComponentConfig& config) {
    auto probe = registries_.probes.create(name, config);
    if (!probe) { spdlog::warn("probe {}: not in this build, skipped", name); return; }
    if (!probe->open(config) || !probe->load() || !probe->attach()) {
        spdlog::warn("probe {}: disabled", name); return;
    }
    if (!loop_.add(*probe, *pipeline_)) { probe->detach(); return; }
    probes_.push_back(std::move(probe));
}

void Daemon::rewire_pipeline_sinks() {
    std::vector<Sink*> ptrs;
    ptrs.reserve(sinks_.size());
    for (const auto& s : sinks_) ptrs.push_back(s.get());
    pipeline_->set_sinks(std::move(ptrs));
}

void Daemon::remove_sink(const std::string& name) {
    const auto it = std::find_if(sinks_.begin(), sinks_.end(),
        [&](const auto& s) { return s->name() == name; });
    if (it == sinks_.end()) return;
    std::unique_ptr<Sink> doomed = std::move(*it);
    sinks_.erase(it);
    rewire_pipeline_sinks(); // Pipeline stops routing to it before it's destroyed, never after
    doomed->flush();
    // destructor runs here: stopping_ = true, join()
}

void Daemon::remove_probe(const std::string& name) {
    const auto it = std::find_if(probes_.begin(), probes_.end(),
        [&](const auto& p) { return p->name() == name; });
    if (it == probes_.end()) return;
    loop_.remove(**it);   // MUST complete before detach() -- see step 5
    (*it)->detach();
    probes_.erase(it);
}
```

`reload_config()`:

```cpp
void Daemon::reload_config() {
    if (config_.config_path.empty()) {
        spdlog::warn("reload: daemon was not started with --config; nothing to reload from");
        return;
    }
    std::string error;
    auto loaded = load_config_file(config_.config_path, error);
    if (!loaded) { spdlog::error("reload: {} -- keeping the current config", error); return; }

    const auto old_probes = selected(config_.probes, registries_.probes);
    const auto new_probes = selected(loaded->probes, registries_.probes);
    const auto old_sinks   = selected(config_.sinks, registries_.sinks);
    const auto new_sinks   = selected(loaded->sinks, registries_.sinks);

    const ConfigDiff probe_diff = diff_components(old_probes, new_probes);
    const ConfigDiff sink_diff  = diff_components(old_sinks, new_sinks);

    // cluster/node/cgroup_root/batch_size/flush_interval/verbose have no live
    // setter anywhere downstream (Pipeline::opt_ and the resolver's cgroup root
    // are construction-only) -- only probes/sinks are hot-reloadable. Warn
    // rather than silently ignore, so an edit to one of these isn't mistaken
    // for having taken effect.
    std::vector<std::string> ignored;
    if (loaded->cluster != config_.cluster) ignored.push_back("node.cluster");
    if (loaded->node != config_.node) ignored.push_back("node.node");
    if (loaded->cgroup_root != config_.cgroup_root) ignored.push_back("slurm.cgroup_root");
    if (loaded->batch_size != config_.batch_size) ignored.push_back("node.batch_size");
    if (loaded->flush_interval != config_.flush_interval) ignored.push_back("node.flush_interval");
    if (loaded->verbose != config_.verbose) ignored.push_back("node.verbose");
    if (!ignored.empty())
        spdlog::warn("reload: {} changed in the file but only probes/sinks hot-reload; restart to apply", fmt::join(ignored, ", "));

    if (probe_diff.empty() && sink_diff.empty()) { spdlog::info("reload: no probe/sink changes"); return; }
    spdlog::info("reload: probes +{} -{} ~{}, sinks +{} -{} ~{}",
        probe_diff.added.size(), probe_diff.removed.size(), probe_diff.changed.size(),
        sink_diff.added.size(), sink_diff.removed.size(), sink_diff.changed.size());

    // Sinks before probes: every probe (re-)added below already has a
    // fully-updated sink list to ship into from its very first event.
    for (const auto& name : sink_diff.removed) remove_sink(name);
    for (const auto& name : sink_diff.changed) remove_sink(name);
    for (const auto& name : sink_diff.added) add_sink(name, new_sinks.at(name));
    for (const auto& name : sink_diff.changed) add_sink(name, new_sinks.at(name));

    for (const auto& name : probe_diff.removed) remove_probe(name);
    for (const auto& name : probe_diff.changed) remove_probe(name);
    for (const auto& name : probe_diff.added) add_probe(name, new_probes.at(name));
    for (const auto& name : probe_diff.changed) add_probe(name, new_probes.at(name));

    if (sinks_.empty()) spdlog::warn("reload: no sinks running -- records will go nowhere until the next reload");
    if (probes_.empty()) spdlog::warn("reload: no probes running -- nothing is being collected until the next reload");

    config_.probes = std::move(loaded->probes);
    config_.sinks = std::move(loaded->sinks);
}
```

`run()`:

```cpp
int Daemon::run(const volatile std::sig_atomic_t& stop, volatile std::sig_atomic_t& reload) {
    ...
    while (stop == 0) {
        if (!loop_.poll(kPollTimeout)) { rc = EXIT_FAILURE; break; }
        if (!resolver_) { rc = EXIT_FAILURE; break; }
        resolver_->tick();
        pipeline_->tick(std::chrono::steady_clock::now());
        if (reload != 0) { reload = 0; reload_config(); }
    }
    ...
}
```

Add `#include <algorithm>` (for `std::find_if`), `#include <spdlog/fmt/ranges.h>`
(for `fmt::join` — confirmed *not* pulled in by the already-included
`spdlog/fmt/fmt.h`), and `#include "core/config/config_diff.h"` to `daemon.cpp`.

### 8. `docs/DESIGN.md` §7

Replace the roadmap sentence with a short description of what actually ships:
`SIGHUP` re-reads the same `--config` file, diffs the *effective* probe/sink sets,
adds/removes only what changed, and leaves `cluster`/`node`/`cgroup_root`/
`batch_size`/`flush_interval`/`verbose` and the attribution cache untouched (with a
warning if the file also changed one of those). Also update the CLI section to
reflect that only `--config` remains.

## Risks, called out explicitly

1. **Removing a sink can stall the main loop** for as long as that sink's own
   `flush()` + destructor `join()` take — for `HttpSink` against a dead endpoint,
   up to roughly `queued_batches × 2×timeout`. Confirmed accepted for v1: this is
   the same class of stall the daemon already accepts at normal shutdown
   (`Pipeline::flush()` calls every sink's `flush()` before exiting), just now also
   possible mid-run, and only when an operator actually removes that sink. Document
   it in the DESIGN.md update; no interface change.
2. **`EventLoop::remove()`'s rebuild can itself fail for a survivor** (`ENOMEM`-class
   only — the same fd worked moments earlier). On failure it logs and leaves that
   binding out of the rebuilt `ring_buffer*`: the probe stays attached and stays in
   `probes_` but silently stops producing events until the next reload or restart.
   Accepted for v1 as an unlikely edge case rather than adding a return type that
   lets `Daemon` also detach+erase affected survivors.
3. **CLI/reload conflict is eliminated by construction**, not handled — since §1
   removes every flag but `--config`, there is no longer a separate "what did the
   command line restrict" state to reconcile against a reload at all.

## Shaping for the future `.so` plugin model (not built now)

`Registry<T>` (`src/core/registry.h`) is already just a name → factory map,
populated once via `register_all()`. Every piece of this plan — `add_probe`,
`remove_probe`, `add_sink`, `remove_sink`, `reload_config` — talks only to
`Registry<T>::create()`/`names()`, never to `register_all()` or the generated
manifest directly. That means a future `dlopen`-based loader can change *how*
`Registry<T>` gets populated (scanning a plugin directory, `dlsym`-ing a `create`
function per `.so` instead of linking `register_<name>()` calls at build time)
without touching anything in this plan. Nothing here needs to anticipate that
further than not closing that door.

## Verification

- `cmake --build` the existing `build/debug` tree; `ctest` (add a real test for
  `diff_components()` in `tests/` — it's pure logic and currently the only
  reload-related piece that's unit-testable without a kernel).
- Manual: start `slurm-tracer --config <path>` with a config naming one probe and
  one sink; edit the file to add a second probe and a second sink; `kill -HUP
  <pid>`; confirm the log lines (`reload: probes +1 -0 ~0, sinks +1 -0 ~0`) and
  confirm both the old and new probe/sink are producing/receiving records; confirm
  `resolver_`'s attribution hit/miss counters (visible at shutdown) were not reset.
  Repeat for removal (should see the `EventLoop::remove` log path exercised) and for
  a "changed" `ComponentConfig` on an existing name.
- `integrations/docker-cluster/` already runs a real `slurmd` + `slurm-tracer`
  pair; a future scenario script there (not required for this plan, but a natural
  next step) could submit a job, `SIGHUP` mid-job to add a probe, and confirm that
  job's later events are still attributed correctly.
