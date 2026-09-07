# Design: M6 — Scale

## 1. The spike, and its one criterion — the derivation key model

The roadmap names this spike and states the failure: *"If keys are not precise, the cache is either
wrong or useless, and every later milestone builds on top of it."*

A derivation key identifies the output of a build step by everything that can change it. Two failure
modes, and they are not symmetric:

- **A key too coarse is a correctness bug** — two different inputs collide on one key, and the cache
  serves the wrong artefact. This is the one that must be impossible, because it is discovered as a
  mysterious wrong result in a downstream milestone rather than as a build error.
- **A key too fine is a performance bug** — the cache misses on changes that could not have affected
  the output. Recoverable, but it makes the cache worthless, which makes the graph pointless.

**The spike's criterion**: a one-asset change invalidates exactly the derivations that depend on it,
and a cold build and a cache-warm build produce **byte-identical** artefacts. Both are M6 exit
criteria, so the spike is testing the milestone's own gate rather than a proxy for it.

What the spike must settle before the graph is built:
- what belongs in a key — tool versions, cook profile, options, input content hashes, and the
  transitive closure of those, and what deliberately does not;
- whether the compiler and its flags are inputs (they are: M0 through M5 pinned LLVM 22.1.8 and the
  Rust toolchain for exactly this reason, and a build that does not treat them as inputs is lying);
- how a key survives a path move, since absolute paths in a key make a cache machine-local;
- and whether non-determinism in any existing cook step defeats byte-identity before the graph is
  written on top of it.

Run the spike outside the repository, as M3's and M5.5's were — `~/cyberdyne-spikes/m6-keys-spike/`.
A prototype in `docs/` fails `quality-layers`, correctly, and the two previous spikes were more
useful for being disposable.

### 1.1 The spike ran, and this is its verdict

`~/cyberdyne-spikes/m6-keys-spike/` — a derivation graph, a content-addressed cache with dependency
re-checking, and eleven experiments over the shape M6 actually builds. Thirty-four checks, compiled
and run under **both** g++ 13.3.0 and clang++ 18.1.3 with byte-identical output, because a spike
whose own thesis is that the compiler is an input would otherwise be asserting that rather than
showing it. The other half of the work was run against the real tree with `cy_cook` and
`cy_import_cli`, not against the model.

**Both criteria are reachable on this tree.** Reaching them needs three additions to the key, and it
needs one existing cook step fixed, because that step is already non-deterministic. The subsections
below are ordered by what each costs if it is ignored.

### 1.2 What belongs in a key, and how it is encoded

| Contribution | Why it is in | What breaks without it |
|---|---|---|
| Producer kind, name and version | Two producers over one source must not share a key | The shader compiler is served the importer's artefact |
| Source content hashes, under **logical** names | Content, so an identical rewrite changes nothing; logical, so a rename does not | Timestamp invalidation; a reorganised project rebuilds whole |
| Target platform and variant | A desktop-BC7 cook is not a mobile-ASTC cook | One platform ships another's textures |
| Cook profile | `client`, `dedicated-server`, `editor` differ in content | E4: the server cook is served the client's cell |
| Every declared option, in **schema order** | An option absent from the key silently does nothing | A quality setting that changes no output |
| **The toolchain fingerprint** | §1.3 — this is the one that is missing | §1.3 |
| Upstream **output** digests | §1.5 | No early cutoff, and the cache is worth a fraction of its cost |

**The encoding is prefix-free, and that is not decoration.** Every contribution is framed: a type
tag, the name's length, the name, the value's length, the value. E1 *demonstrates* the alternative
rather than asserting it — `{compress="ionlevel", ""="9"}` and `{compressi="on", "level"="9"}` hash
to the same value under naive concatenation, and a cache keyed that way serves one cook's artefact to
another with no diagnostic at all. `cy::assets::DerivationKeyBuilder` already frames correctly, and
so does `cy::shader::derive_cache_key` by a different mechanism. Nothing new is needed here; it is
written down so that nobody later "simplifies" it.

**Contributions are hashed in order, so order is part of the contract.** A producer that adds its
fields wherever each input happens to be discovered will miss its own cache the first time two runs
discover them in a different order (E10). Two rules follow, and both are cheap now: build each key in
**one function**, and iterate options by **name**, never by insertion.

### 1.3 The compiler, its flags and its libraries are inputs — and nothing in the tree records them

This is the correctness bug the spike was commissioned to find, and it is present.

`import_derivation_key` (`tools/import/src/importer.cpp`) builds the only `assets::DerivationKey`
anything in this tree computes — the shader cache computes a second, different key, see §1.8. Its
complete contribution list is: the importer's name and a hand-maintained `u32` version, the source
content hash, the variant, the cook profile, and the declared options. **No compiler, no compiler
flags, no third-party library version.**

**Demonstrated, not argued.** `cy_import_cli` was built twice from the same source at two
optimisation levels — `Development` (`-O2 -g`) and `Debug` (`-O0 -g`) — and each ran over the same
glTF:

    Development / -O2 : 04a6fff1506488810010e25eca5290b114ae67c6fccc8ca88db4b749e4cbc512.cyderive
    Debug       / -O0 : 04a6fff1506488810010e25eca5290b114ae67c6fccc8ca88db4b749e4cbc512.cyderive

The same key. Pointed at the other build's cache, the `-O0` binary reported `1 hit, 0 miss` and was
served the `-O2` binary's artefact. The payloads happened to agree for this asset, so nothing broke —
but **the key cannot tell, and would have served it either way.** The cache is blind to the toolchain
by construction rather than by luck.

**The compression library changes the artefact bytes, and it is switchable by a CMake option.** Every
cooked payload is written `PayloadForm::Compressible` and compressed with zstd at
`CompressionLevel::Default` (level 3). `deps/manifest.toml` pins zstd 1.5.7 but declares
`system_package = true`, and `CY_SYSTEM_ZSTD` selects between the pin and the host's. Both are on the
development machine and they disagree — measured, round-trip verified in both directions:

| Input, 256 KiB | zstd 1.5.7 (the pin) | zstd 1.5.5 (this host) |
|---|---|---|
| Synthetic, cooked-block shaped | 856 bytes | **792 bytes** |
| Highly repetitive | 52 bytes | 52 bytes |

Both decompress correctly; they are simply not the same bytes. The second row is why this would slip
past a casual test. One CMake option changes every cooked artefact in the project, and no key notices.

**The C++ compiler is undeclared and, today, single-valued — the most dangerous state of the three.**
`deps/host-tools.toml` pins `clang-python` 18.1.8, `libclang` 18 and Rust 1.95.0. It does not pin the
C++ compiler. `CMakeCache.txt` records `CMAKE_CXX_COMPILER=/usr/bin/c++`, which is g++ 13.3.0, while
clang++ 18.1.3 and clang-tidy at LLVM 22.1.8 are both installed. Configuring with clang++ 18.1.3 does
not merely produce different artefacts: **it does not build.** `-Werror` catches
`-Wunused-private-field` at `src/core/assets/include/cy/core/assets/watch.h:208` and seven
`-Wdouble-promotion` conversions at `tools/import/src/mesh.cpp:81–93` that gcc does not diagnose. So
"which compiler produced this artefact" has one answer today and nowhere to record it — and the day
those eight diagnostics are fixed and someone builds with clang, every artefact already in a shared
cache silently becomes suspect. Left unfixed here, because they are not the spike's files; recorded
so that M6 decides deliberately.

**And the importer's own version is a hand-maintained integer.** `info.version` is bumped by whoever
remembers to. E4 shows what that permits.

**What is required.** A `ToolchainFingerprint` contributed to every key **by construction** rather
than by each producer remembering: the C++ compiler's identity and version; the flags that can move a
float (`-O`, `-ffp-contract`, `-ffast-math`, the target architecture, any `-march`); the standard
library; `libclang`'s version wherever reflection metadata is an input; `rustc`'s version for any
Rust-produced artefact; and the version of **every third-party library linked into the producer** —
zstd today, ufbx and xatlas from this milestone. Those versions are already declared in
`deps/manifest.toml` and `deps/host-tools.toml`, so the fingerprint should be **generated from those
files at configure time**: adding a dependency without adding it to the key then becomes impossible
rather than merely discouraged, and task 0b's attribution gate and this key model become the same
mechanism seen from two ends.

One inversion worth not repeating: `cy::shader::CacheKeyInputs` **does** carry `compiler_name` and
`compiler_version`. The shader cache got this right and the asset cache did not.

### 1.4 A key survives a path move only if no path is in it

Verified on the real importer rather than only in the model. The same glTF imported at one absolute
root and then at another, against one shared cache, gave `0 hit, 1 miss` then `1 hit, 0 miss`, one
cache entry, and six byte-identical payloads. `cy_cook` is likewise byte-identical across a moved
source root, a nested source root, and filenames renamed so that the read order reverses. Moving a
file *within* a project rebuilds nothing either, because the key holds a logical name and the content
and never the path.

The rule that makes this true must hold for discovered dependencies as much as for declared inputs:
**every path in a key or in a dependency record is project-relative**, and the resolver that
re-digests a dependency name applies the same resolution rule on every machine. A dependency name
that resolved differently on CI than on a developer's machine would be a hit served for different
content — the same class of bug as a coarse key, arriving through the back door.

E5 measures the cost of getting it wrong: with absolute paths in the key, every node with a file input
misses — the four importers and compilers, which are the expensive half — while early cutoff spares
the four below them, so the shared cache becomes dead weight *silently*.

### 1.5 A downstream key holds its upstream's OUTPUT digest, not its upstream's key

The decision that is expensive to reverse, and the one the graph must be built on from its first
commit. Two candidate models:

- **Deep input keys.** A node contributes its upstream's *key*. The whole graph can be keyed before
  anything runs, which is why it is what a first implementation reaches for.
- **Upstream output digests.** A node contributes its upstream's *output content hash*. This requires
  topological evaluation, since a downstream key is not known until its upstream has run or hit.

They differ in **early cutoff**, and E7 measures it. An edit that reaches a node without changing what
it produces — a comment an importer normalises away, whitespace in a material graph, a header edit
that does not alter generated code — rebuilds **three** nodes under deep input keys and **one** under
output digests. On a real graph that difference is the whole value of the cache.

So the model is two-level, which is what `build-and-packaging`'s "an artefact with a given identity
SHALL NEVER change" already implies:

- a node has an **input key**, computable before it runs, answering *what did this computation
  produce*;
- its output is **content-addressed**, and that digest — not the input key — is what downstream keys
  and the patcher's chunk table hold.

The cost is that the build service must evaluate in dependency order and cannot key the whole graph up
front. That is how a build service works anyway, and far cheaper than retrofitting early cutoff once
seven producers depend on the other shape.

### 1.6 What deliberately does not belong in a key

The output directory, the project root, the wall clock, the hostname, the user, the worker count,
which cache tier answered, the build's own identity, and an artefact's eventual path inside a package.
None of them can change a byte of a correct artefact, and each would make the key unique per run or
per machine.

E6 turns that failure into a number rather than a warning: with a build counter in the key, five
identical builds produce **0 hits** and leave **40 cache entries** where 8 nodes' worth of content
exists. A key too fine does not merely fail to help — it makes the cache a leak.

Verified as a negative on the real importer, and worth recording because it is the kind of thing
assumed rather than checked: `cy_import_cli --jobs 1` and `--jobs 16` over twelve assets produce the
same 72 artefacts under the same keys. **Worker count does not leak.**

### 1.7 Task 1.5's answer: one existing cook step is not deterministic

Every producer of derived data in the tree was audited by running it twice and diffing, not by reading
it:

| Producer | How it was checked | Verdict |
|---|---|---|
| `cy_cook`, documents → `.cypak` | two runs; a moved root; a nested root; a rename reversing the read order | **Deterministic** in all four; the sorted directory and chunk table earn their keep |
| `cy_import_cli`, cooked payloads | cold vs cache-warm; `--jobs 1` vs `--jobs 16`; two absolute roots | **Deterministic.** Criterion 2 holds on the real tool |
| `cy_import_cli`, asset **identity** | two cold imports of one glTF from two clean checkouts | **NOT deterministic** — below |
| Reflection and header generation | `just generate-check`: generates twice, diffs, greps for absolute paths | **Deterministic**, already gated. The pattern the cook steps need and lack |
| Shader compilation | read: `build-shaders` is `_not-implemented`; Slang compiles in-memory strings under module names | **No cook step exists yet.** It must land with a two-run diff, not gain one later |
| zstd payload compression | §1.3 | Deterministic **for a fixed library**; the library is neither pinned in practice nor in any key |

**The one that fails.** `assets::mint_asset_id()` (`src/core/assets/src/identity.cpp:138`) draws 128
bits from `std::random_device`, and `bind_sub_assets` (`tools/import/src/sidecar.cpp:499`) calls it
for every sub-asset whose name is not already bound in the committed `.import` record. Two cold
imports of `samples/05-editor-session/project/assets/lamppost.gltf` from two clean checkouts — same
machine, same binary — produced six artefacts each, under six entirely different names:

    327e01fc…  971dadd3…  9d371c5d…  a73348e5…  b47ec7f1…  e89658e6…
    2ac0acc8…  9ccb7f2c…  9ffb3eda…  b0a95f9c…  ea2e357c…  fb3a88bd…

The six *payloads* were byte-identical pairwise, so the damage today is confined to the file names and
the registry. **M6 is the milestone that detonates it.** `serialization-and-prefabs` at Complete means
a prefab references its meshes by `AssetId` and a world cell references its prefabs by `AssetId`. From
that commit the identity is *inside* the payload, and two cold builds of one project stop producing
the same bytes.

The design is not wrong: an id must be drawn once and then be stable, and the `.meta` and `.import`
sidecars are authoritative metadata that belong in source control, exactly as `identity.h` argues.
What is missing is that **nothing enforces it.** Three things close it, all cheap now:

1. **A cook profile in which minting is refused.** A shipping or CI cook that would mint an id fails
   naming the asset, rather than minting one and producing an artefact nobody can reproduce. The
   remedy for the diagnostic is to commit the sidecar, which should have happened anyway.
2. **The sidecar is a declared input** to the import derivation, not an incidental file beside it.
3. **A two-run determinism gate over the cook**, in the shape `just generate-check` already has: cook
   twice into two directories, diff, and grep the result for absolute paths. That gate is
   `build-and-packaging`'s "Determinism SHALL be verified in continuous integration", and it is the
   only thing that keeps this from recurring in a producer nobody has written yet.

E8 shows why the third cannot be replaced by trusting the cache: a non-deterministic step plus a cache
is *worse* than the step alone, because the cache freezes whichever machine missed first, and the
artefact that ships is then decided by a race rather than by the content.

### 1.8 There are already two caches, and both specifications require one

`cy::assets::DerivedCache` (`src/core/assets/`) and `cy::shader::CacheTier` with `derive_cache_key`
(`src/backends/shader/`) are two content-addressed three-tier caches, with two key types, two
encodings and two tier abstractions. `asset-import-pipeline` is explicit — "the import cache SHALL NOT
be a separate mechanism from the cache used for shaders … there SHALL be one cache covering all
derived data" — and `build-and-packaging` says the same of the derived data cache.

They disagree about substance, not only shape: the shader key carries the compiler's name and version
and the asset key carries none. Whichever survives, there must be **one**, and its key must be the
union of what each got right. Left in place they will disagree about eviction, about tiers, and about
what a hit means — which is how every engine that grew three caches got them.

### 1.9 What the graph's first commit must do

1. One key type and one cache for every producer. Fold `cy::shader::CacheKeyInputs` into
   `cy::assets::DerivationKeyBuilder` rather than adding a third mechanism.
2. A `ToolchainFingerprint` generated from `deps/manifest.toml` and `deps/host-tools.toml` at
   configure time and contributed to every key by construction, so that adding a dependency without
   adding it to the key is impossible rather than discouraged.
3. Downstream keys hold upstream **output digests**; artefacts are content-addressed and immutable.
4. Every path in a key or a dependency record is project-relative, resolved by one rule everywhere.
5. A cook profile that refuses to mint an identity, and the sidecar declared as an input.
6. A two-run determinism gate — cook twice, diff, grep for absolute paths — landing with the *first*
   cook step rather than after the seventh.
7. Undeclared reads are reported. No key can catch one: E9 shows the build serving a stale artefact
   and reporting success. That is why `build-and-packaging` makes an undeclared read a **defect**
   rather than a miss, and why the verification mode has to be built rather than planned.

## 2. Worlds load, and the editor is the first consumer

The order matters. `serialization-and-prefabs` reaching Complete is what gives a world a schema and
nodes; `world-partition-and-streaming` is what makes that world larger than memory. The editor is
downstream of both and should be wired **as soon as opening a world produces content**, not at the
end of the milestone — because the editor is the fastest way to see that streaming, activation and
the persistence overlay are wrong.

Concretely: `DocumentService::open` must produce a document whose schema declares the engine's
registered component types, so `TransformBinding::of_schema` finds a `Transform`. Everything M5.5
built downstream of that — selection, the gizmo's four modes, per-axis entry, one transaction per
manipulation — is already tested against documents that declare one. This is a connection, not new
interaction work.

## 3. Residency and activation are separate, and a test must prove it

`residency` is "shared policy with separate storage". The requirement that keeps it honest is the
M6 exit criterion: **a test holds bytes resident with simulation off.** If residency and activation
cannot be separated, streaming becomes an all-or-nothing operation and the frame budget goes with it.
Design the policy so that being resident and being active are two independent facts about a cell from
the first commit; retrofitting that separation after HLOD and the persistence overlay depend on it is
the kind of change that is a migration rather than an edit.

## 4. The save is the overlay

`save-and-persistence` does not add a serialisation path beside the streaming one. The persistence
overlay that world partition already maintains **is** the save: scopes and traits decide what
persists, persistent identity survives a cell unloading, and atomic generations make a save that is
interrupted by `kill -9` either fully applied or absent. The exit criterion is exactly that — a
save/load round-trip of an *unloaded* region's state, and atomic generations under `kill -9`.

## 5. What must not be retrofitted

Pinned to this milestone because they are cheap now and are migrations later:

| Invariant | Why it cannot wait |
|---|---|
| Derivation keys include the toolchain | A cache that ignores the compiler serves artefacts built by a different one |
| Cells are cooked in ECS-native form | Cooking to an intermediate and converting at load makes streaming cost proportional to content |
| Residency separate from activation | Every later system that streams assumes it |
| Persistent identity is stable across unload | A save written before this is a save that cannot be migrated |
| Immutable artefacts | A mutable artefact makes every downstream key a lie |
