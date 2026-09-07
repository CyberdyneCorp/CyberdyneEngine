# tools/build/ — the derivation graph and the build service

`build-and-packaging` at **Working**, M6 section 7. Five milestones were built by a script that is
correct because it is re-run from scratch. That does not survive a multi-kilometre world, a cook
profile per platform, or a patch. This is the graph that replaces it.

Two targets, for the reason `tools/cook/` has two: `cy_build_graph` is a library with no `main` and
no argument parsing, and `cy_build` is a thin front end over it. The interesting assertions are on
the report and on the store, never on an exit code.

## What is here

| File | What it decides |
|---|---|
| `include/cy/build/toolchain.h` | The toolchain fingerprint, **generated from `deps/manifest.toml` and `deps/host-tools.toml` at configure time** and contributed to every key by construction |
| `include/cy/build/graph.h` | Nodes, declared inputs and outputs, evaluation order, "what does this change reach?", "why is this in the build?" |
| `include/cy/build/key.h` | The derivation key, built in **one** function |
| `include/cy/build/artefact_store.h` | Immutable, content-addressed artefacts |
| `include/cy/build/producer.h` | What runs a node, and how its reads and writes are verified |
| `include/cy/build/service.h` | The build service: the graph, the cache, the workers, cancellation, structured events |
| `include/cy/build/package.h` | Packages, install bundles, provenance, the content audit |
| `include/cy/build/patch.h` | Chunk-level patching, and an installation that survives being interrupted |
| `include/cy/build/description.h` | `cybuild 1` — the graph as text |

## The four decisions that are expensive to reverse

They are argued at length in `openspec/changes/implement-m6-scale/design.md` §1 and restated in each
header. In short:

1. **The toolchain is in every key.** The spike built one importer at `-O2` and at `-O0` and got the
   identical key; one build was served the other's artefact with a reported cache hit. The
   fingerprint is *generated* from the dependency manifests, so adding a dependency without adding
   it to the key is impossible rather than discouraged — and `tools/build/CMakeLists.txt` refuses to
   emit an empty library list.
2. **A downstream key holds its upstream's OUTPUT digest, never its key.** An edit that reaches a
   node without changing what it produces rebuilds three nodes under deep input keys and one under
   output digests. The cost is that the service must evaluate in dependency order, which it does
   anyway.
3. **Artefacts are immutable**, by name (the name is the digest), by mode (0444) and by audit
   (`verify_all`). A mutable artefact makes every downstream key a lie.
4. **An undeclared read is a defect**, and no key can catch one. A producer is handed a
   `NodeContext` and no filesystem, so a name that was not declared cannot be resolved at all.

## Running it

```sh
just build-engine --profile dev -D CY_BUILD_TOOLS=ON
BUILD=build/dev/tools/build/cy_build

$BUILD toolchain                       # what every key contributes about the toolchain
$BUILD build     --project <dir> --description <file> --out <store> --cache <dir> \
                 --package <file>      # run the graph, and write a package manifest
$BUILD explain   --source <name>       # which nodes a change to that source would reach
$BUILD audit     --node <name>         # why it is in the build, and what references it
$BUILD determinism                     # build twice into two roots and diff every artefact
$BUILD patch     --from <a> --to <b> --out <patch>
$BUILD install   --install <dir> --package <file> --artefacts <store>
$BUILD apply     --install <dir> --patch <file> --artefacts <store> [--crash-at <stage>]
$BUILD verify    --install <dir> | --artefacts <store>
```

`--crash-at <begin|fetch|verify|commit|switch>` kills the process at that stage with `_exit`, which
is how the exit criterion *"a patch applies atomically and rolls back cleanly when interrupted"* is
executed rather than argued. `--stop-at` does the same thing softly, returning through the rollback.

Two `just` recipes wrap the tool, and M6 filled them in: they had said "M12 — build-and-packaging"
since M0, and the roadmap moved the capability to M6.

```sh
just content-package --description <file> --project <dir> --out <store> --package <manifest>
just content-patch   --from <a.cypackage> --to <b.cypackage> --out <patch>
just content-patch   --apply --install <dir> --patch <file> --artefacts <store>
```

## The suites, and what each is for

| Suite | What it holds |
|---|---|
| `unit.build_graph` | Topology, cycles, the key model (including the collision E1 demonstrates), the three text formats. No filesystem |
| `integration.build_service` | Both build exit criteria, undeclared reads, a mutated artefact, cancellation, parallelism, and **a service destroyed while its workers are still running** |
| `integration.build_patch` | Packaging, a chunk-level patch, and an application interrupted at every stage — softly, and by killing the process |

## What is deliberately not here yet

* **Remote workers.** `BuildConfig::distributed` is read and reported; there is no worker pool
  behind it. `build-and-packaging` requires that "WHEN no remote workers are reachable THEN the
  build SHALL execute locally with no change in result", and that degradation is the state the
  milestone ships in.
* **Watching source files.** The service invalidates from content on every build. A file watcher
  (`cy::assets::Watcher` exists) turns that into a service that reacts; nothing in the key model
  changes when it lands.
* **Manifest signing and encryption.** `build-and-packaging` requires established implementations
  and platform facilities; `core/crypto` has none of them yet, and inventing one here is exactly
  what that requirement forbids.
* **The two existing caches are still two.** `cy::assets::DerivedCache` is the one this service
  uses; `cy::shader::CacheTier` is untouched, because `src/backends/shader/` is not this change's to
  edit. design.md §1.8 records what merging them costs and why the merged key must be the union of
  what each got right.
