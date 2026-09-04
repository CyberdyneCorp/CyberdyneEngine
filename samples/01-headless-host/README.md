# `samples/01-headless-host` — the M1 milestone artefact

One program that loads a package from the virtual filesystem, runs a parallel job graph over
reflected data, reports its memory budget tree, and shuts down deterministically. Tasks 5.1–5.4.

```
just run-sample headless-host                        the defaults: 4096 entities, 8 frames
just run-sample headless-host --host.frames=64       any declared setting, from the command line
just run-sample headless-host --host.memory-profile=handheld
just run-sample headless-host --content /tmp/scratch --keep-content
just run-sample headless-host --help
```

A run says what it did and exits zero:

```
01-headless-host: startup  memory reflect jobs async vfs assets
01-headless-host: settings entities=4096 blocks=8 frames=8 memory-profile=desktop (from EngineDefault)
01-headless-host: project  CyberdyneEngine 0.0.0, 1 modules declared, manifest absent
01-headless-host: package  entries=8 chunks=8 records=544768 B  file=52822 B  deduplicated=0 B
01-headless-host: loaded   assets=8 bytes=544768 records=8192 partitions=8
01-headless-host: schedule systems=3 batches=2  [decay drift] [summarise]
01-headless-host: frames   8 run, 1172 entities retired through the command buffer
01-headless-host: assets   loaded=8 coalesced=0 placeholders=0 resident=0 (0 B)
01-headless-host: jobs     workers=23 tasks=57
01-headless-host: budget   profile=desktop worst=ecs at 0.03% of its budget
01-headless-host: budget   engine     hard target   4096 MiB  live   253472 B  peak  2350832 B   0.01%
01-headless-host: budget   ecs        soft target    512 MiB  live   180224 B  peak   180224 B   0.03%
01-headless-host: reflect  types=2, 0 reflected lookups inside a hot region
01-headless-host: checksum 7e1c8061aa777107
01-headless-host: shutdown assets vfs async jobs reflect memory
01-headless-host: exit 0 (clean)
```

## Why this program and not four

Each M1 module already has its own suite, and each of those suites passes against a world it built
for itself. What none of them can test is the seam: an identifier written by the serializer and read
by the scheduler, an allocation attributed by one module and reported by another, a load that begins
on the async service and finishes on a worker. This sample is one program in which all of that has
to agree, which is why it is the milestone's gate rather than a demonstration.

Three files, one phase each.

| File | What it owns |
|---|---|
| `main.cpp` | The host: the settings, the six services and the order they come up and go down in, the phases, and the report. |
| `content.cpp` | The package: cooking it, mounting it, loading it, and decoding its records back into components. |
| `simulation.cpp` | The world: the field bindings, the three systems and their access declarations, and the frame. |

## The five things it actually proves

**A reflected record survives the round trip by identifier.** The package's payload is a run of
`cy::reflect` records. A record names its type by `TypeId` and its fields by `FieldId`; no name and
no byte offset is in it. The stage binds the same identifiers — spelled once, in `simulation.cpp`,
against `identity/manifest.toml`. Rename a field upstream and the manifest changes, the generated
strings change, and neither this package nor this program does.

**The control-plane rule holds where it is easy to break.** Every field is resolved once, at setup,
by a reflected lookup that yields a `TypedAccessor` — a byte offset with no registry pointer and no
lookup on it. Every system body declares `CY_REFLECT_HOT_REGION`, so a lookup that crept back into
the loop would be *counted*, in every configuration, rather than asserted about in two of them. The
run prints the count, and it is zero.

**Parallelism is derived, not declared.** `decay` declares a write of `Health`, `drift` a write of
`Placement`, `summarise` a read of both. Nobody writes down which may run together: the schedule
derives it, and prints the plan it derived — `[decay drift] [summarise]`. Change a declaration and
the plan changes with no scheduling code edited.

**Memory is attributed and compared against a target.** The entity arrays are taken from
`MemoryDomain::Ecs`, and the budget report at the end is the platform profile's apportionment beside
what was actually taken. `--host.memory-profile=handheld` changes the apportionment and no code.

**Shutdown is the exact reverse of startup, every time.** The six services live on a stack: there is
one order written down, and teardown unwinds it. `tests/smoke/test_headless_host.cpp` runs the whole
program a hundred times, in a hundred processes, and requires the startup journal, the shutdown
journal and the simulation's checksum to be identical in all of them. The checksum is also identical
across `debug`, `dev`, `profile` and `release`.

## Where it stands in for something that does not exist yet

**It cooks its own package.** The cooker is M2, so `sample::cook()` writes the `.cypak` before the
program mounts it. That is the one place the artefact substitutes for a missing part of the engine,
and it is deliberately a separate phase with no other caller: everything after it goes through the
virtual filesystem and the asset system exactly as a shipped build would. When the cooker lands,
`cook()` is deleted and nothing else here changes.

**Its asset ids are derived, not minted.** A cooker mints an id once (`mint_asset_id()`) and records
it in the sidecar. This content is regenerated from scratch on every run and nothing persists a
reference to it, so `block_id(n)` derives one instead and the run stays reproducible.

**The asset system's own allocations land in `MemoryDomain::Engine`**, not in `Assets`:
`AssetSystem` takes no domain, and the payload buffers are allocated on worker threads where a
main-thread `AllocatorScope` does not reach. The report is honest about what it can see — the `Ecs`
row is a real figure from a real attribution — but the `Assets` row is empty in a program that
plainly loaded assets. Giving `AssetSystemConfig` a domain is the fix, and it belongs to
`src/core/assets/`.

**Nothing is over budget, so the pressure path is not exercised here.** Raising pressure from an
over-budget domain is task 6.5's gate and has its own test in `src/core/memory/`.

**Two reported figures are not reproducible, and only one of them had to stay that way.** A domain's
*peak* is a high-water mark over allocations several threads make at once, so it legitimately differs
by a few kilobytes between runs; the smoke test excludes the budget rows from its comparison for
that reason and compares live bytes, which do not move. The *task count* looked like the same kind of
figure and was not: `JobSystemStats::tasks_executed` is incremented **after** the release store that
unblocks `wait()`, so a host that waits for its last task and reads the counter in the next statement
can miss it — an undercount of exactly one, about once in two hundred runs on a loaded machine, which
is roughly a two-in-five chance of failing a hundred-run gate. The sample reports
`tasks_submitted` instead: it is incremented before the body runs, so it is ordered before that
release store and is exact for every task this program waited on, and every task here is waited on.
The line stays inside the compared text. Reporting a figure that *is* ordered was the fix; excluding
a second line from the comparison would have been the accommodation.

**`ModuleRegistry` is not used to order the services.** It orders by `(level, name)`, which is what
makes a *project's* module order independent of registration order — but it is not a dependency
order, and these six services genuinely depend on one another (`assets` needs `jobs`, `async` and
the mounted namespace). A host that owns real service dependencies sequences them itself; the
registry is for the modules a project declares, and `src/runtime/` owns the engine's eleven stages.

**Windows and macOS are unverified.** The sample adds no platform-conditional code beyond reading
`TMPDIR`/`TEMP` for its default content directory, but it has only been run on Linux.

**Governed by**: `delivery-roadmap` (milestone artefacts), `core-type-system`,
`core-memory-and-containers`, `core-jobs-and-concurrency`, `core-assets-and-io`,
`project-and-plugins`, `engine-architecture` (deterministic startup and shutdown).
