# `src/core/assets` — asset identity, the virtual filesystem, packages, and loading

Layer 0, target `cy::core-assets`, headers `<cy/core/assets/*.h>`, namespace `cy::assets`.
Section 3.3 of `openspec/changes/implement-m1-substrate/tasks.md`, governed by `core-assets-and-io`.

**This capability reaches SEED at M1, not Working.** What is here is identity, the virtual
filesystem, the package format's read path, asynchronous loading, file and directory access, the two
serialization forms, and compression. What is deliberately absent is listed under *Seams* below.

**The derived-data cache joined it at M5** (task 5.1). `asset-import-pipeline` and
`build-and-packaging` both require ONE content-addressed cache over every derived artefact — cooked
assets, shaders, material programs, geometry and texture pages — rather than one per producer, so it
is here at layer 0 beneath all of them rather than inside the importer. `derivation.h` is what
addresses an entry and `derived_cache.h` is what stores it; `tools/import/` is its first client.

**Two things joined it at M7** (tasks 1.1, 1.2, 2.1, 2.2).

*Partial residency* (`streaming.h`) is the "Streaming" requirement M6 planned and did not touch: an
asset that declares a ladder of texture mips, mesh LODs or audio chunks is resident a level at a
time, driven by a budget and by renderer feedback, and **a not-yet-resident level falls back to the
highest resident one rather than blocking a frame**. It is on in every build — `AssetSystem::start`
starts it with the configuration's residency budget and `update()` drives it — because the
requirement is that the engine supports partial residency, not that a caller can assemble it.

*The toolchain fingerprint* (`toolchain.h`) moved here from `tools/build/`. M6 built it at layer 7,
where `cy::import` and `cy::shader` could not reach it, so the function that actually cooks content
still computed a key blind to its own compiler and M6's closing gate re-measured the original defect
as **1 hit, 0 miss** across two importer binaries sharing one cache. One cache needs one key, and one
key needs its inputs reachable from every producer that computes one — which is layer 0.

**Hot reload joined it at M3** (task 1.4, carried forward from M2's gate). `FileWatcher` watches the
virtual filesystem and reports added, modified and removed paths; `AssetSystem::reload()` replaces a
resident asset's bytes **inside the object every `Ref` already points at** and tells registered
dependents so they can rebuild what they derived. Both halves are exercised by
`integration.assets_watch`, including the specification's "reload failure keeps the old asset".

## The map

| Header | What it owns |
|---|---|
| `path.h` | `VirtualPath` — normalised, case-sensitive, traversal-proof, fixed capacity |
| `hash.h` | `ContentHash`, `ContentHasher` — BLAKE3 behind an engine-owned type |
| `identity.h` | `AssetKind`, `VariantKey`, `AssetMeta` and its sidecar text form, `AssetDatabase`, placeholders |
| `derivation.h` | `DerivationKey` and its builder — what makes two pieces of derived data the same piece |
| `derived_cache.h` | The **one** derived-data cache: local, shared and remote tiers, recorded dependencies, disposable by construction |
| `compression.h` | `CompressionMethod`, block and **seekable framed** compression, the recompression policy |
| `file.h` | `File`, `MappedFile`, `fs::` directory operations, `fs::write_atomic` |
| `vfs.h` | `Mount`, `DirectoryMount`, `MemoryMount`, `RemoteMount`, `VirtualFileSystem` |
| `package.h` | The `.cypak` format: `PackageReader`, `PackageWriter`, `PackageSet`, `PackageMount` |
| `serialization.h` | The binary envelope and the text form over reflected data |
| `asset_system.h` | `AssetSystem`, `AssetData`, `LoadRequestId`, the retention policies, and `reload()` |
| `streaming.h` | `StreamingSystem` — partial residency for mip levels, mesh LODs and audio chunks under a budget (**M7**) |
| `toolchain.h` | `ToolchainFingerprint` — what compiled a producer, contributed to every derivation key (**M7**) |
| `watch.h` | `FileWatcher` — polls the namespace, reports what changed, debounces a file still being written |
| `diagnostics.h` | The layer's counters, on the M0 trace |
| `assets.h` | The umbrella |

## Five decisions worth knowing before changing anything here

**1. An asset id is minted, never derived.** `mint_asset_id()` draws 128 random bits. There is no
function that makes an id from a path, a name or a content hash, because every one of those changes
under the edits the id exists to survive. The content hash is a *separate* value with a separate
job: cache validation, incremental builds and patch diffing.

**2. A package entry is a reference to a content-addressed chunk.** Two entries with identical
payloads name one chunk, so the bytes are stored once; an entry may name a chunk another package
holds, which is how a patch ships only what changed. `PackageSet` resolves across packages.

**3. Packages live in the path namespace.** A package mount serves `packaged/<32 hex id>[.variant]`.
That is not cosmetic: it puts patch masking and mount priority on the **same** machinery as every
other mount, rather than giving packages a second resolution order that could disagree with it.
`Mount::as_package()` is the one place the namespace exposes a mount's richer identity — the engine
is built with `-fno-rtti`, so a `dynamic_cast` is not available and a named hook says what it is for.

**4. A worker never blocks.** The read stage runs on `cy::jobs::AsyncService`, the one thread where
blocking is legal; decompression and deserialization are jobs gated on the read's handle. The load
path calls no blocking filesystem function from a job body, and
`tests/test_asset_system.cpp` asserts `cy::jobs::blocking_violations()` is unchanged across a full
load — a counter compiled into **every** configuration, so the assertion is not vacuous in Profile
and Shipping.

**5. Framing is what makes a partial read partial.** A compressed payload is a sequence of
independently decompressible frames plus an index. `decompress_range` takes a *reader callback*
rather than a buffer precisely so that the bytes outside the requested frames are never read from
disk at all. `PackageReader::read_entry_range` reports `bytes_from_disk` and `frames_touched`, and
the tests assert on both.

## Seams — where the parts that are not here will attach

| Not here | Where it attaches | Milestone |
|---|---|---|
| Cooking, importers, source-format parsers | `PackageWriter::add` takes bytes that are already cooked | M2 |
| ~~Streaming, per-mip residency, renderer feedback~~ | **Closed at M7** by `streaming.h`. `AssetSystem` retention is still about *when memory goes back*; `AssetSystem::streaming()` is about what fraction of an asset is resident | M7 |
| GPU upload | `AssetSystemStats::uploads_skipped` counts the stage that is skipped | M3 |
| A native change-notification backend (inotify, ReadDirectoryChangesW, FSEvents) | `FileWatcher` polls the `VirtualFileSystem`, which is layer 0; a native backend is platform code and belongs behind the platform seam. Its callers' contract — call `poll()`, be told what changed — does not move | when someone needs the latency |
| Reloading a **package-backed** asset | `AssetSystem::reload` refuses one by name: an entry inside a cooked package reaches its bytes through chunk framing, decompression and a dependency pass that only the load pipeline implements. Closing it means restarting that pipeline into the existing slot and swapping at `publish()` | when a cooked package is iterated on |
| Encryption | `PackageFlags::EncryptedDirectory` / `EncryptedPayload` are defined and `open` **refuses** them | when key management exists |
| LZ4, Deflate | `CompressionMethod` declares them; every entry point refuses them by name. Neither codec is pinned in `deps/manifest.toml`, and adding a dependency is a manifest decision | when one is pinned |
| SHA-256, HMAC, AES-GCM, CSPRNG | `hash.h` is the shape a `core/crypto` takes; nothing at M1 signs or encrypts | M2+ |
| Typed `load<T>` over a factory registry | `AssetData` is the cooked blob; a typed layer needs types, which arrive with the ECS and the renderer | M2/M3 |

## One bug found and fixed here, with the regression test that pins it

A retired asset's single reference is held by the **retention policy** rather than by any `Ref`:
`on_last_reference` resurrects the count from zero back to one so that a `TimeDelayed` or
`BudgetBased` policy can keep the asset. Reviving that slot — a second request for the same asset —
used to clear the retired flag *without taking that reference over*, so the asset was thereafter
owned by nothing: dropping the reviving `Ref` never brought the count to zero, `update()` never saw
the slot as retired again, and the asset lived until the process exited. It was invisible except
under LeakSanitizer (20 752 bytes in 4 allocations across two cases).

`AssetSystemImpl::revive()` is now the one place a retired slot comes back, and it *moves* the
resurrected reference into `pending`. The regression test is `A retired asset that is revived and
released is collected again` in `tests/test_asset_system.cpp`, and it asserts the observable half
rather than needing a sanitiser: with a zero retention delay, the asset must be gone after the next
`update()`. Verified against the defect — with `revive()` reduced to clearing the flag, the case
fails on `resident_assets` and `resident_bytes`.

## Where this is thinner than the specification

* **Strings, containers and nested reflected structs do not serialize.** `reflect::FieldKind` covers
  scalars and enumerations at M1; a field outside that is reported by name rather than dropped. They
  cross the boundary as `cy::Var`, which is the values module's work.
* **`AssetMeta` carries no importer settings.** Those belong with the importer that reads them, and
  there is no importer.
* **The remote mount has no transport.** `RemoteFileProvider` is the interface; there is no socket,
  no protocol and no host discovery, because none could be tested on one machine and a protocol
  nobody has spoken is a protocol that is wrong.
* **Windows and macOS are UNVERIFIED.** `src/file.cpp` is POSIX for positional reads and memory
  mapping, behind `CY_ASSETS_POSIX_IO`, with the non-POSIX branch returning `Unsupported` rather
  than doing something else. The Windows implementation is `CreateFileMapping`/`MapViewOfFile` and
  `ReadFile` with an `OVERLAPPED` offset; it is deliberately not written here, because prose that
  has never compiled is worse than an honest refusal.
* **The binary form refuses a big-endian host at compile time.** The format is little-endian by
  definition and nothing here has ever run on a big-endian machine, so the `static_assert` says so
  rather than the code claiming a portability it has not demonstrated.

## Tests

`tests/CMakeLists.txt` declares four suites, 92 cases. **18 of the specification's 23
`#### Scenario` blocks have a test case named after them.** The five that do not are the ones whose
subject is not built at M1, and each is named here rather than left to be discovered:

| Scenario | Why it has no test |
|---|---|
| Shipping build has no source assets | Cooking and packaging a shipping build are M2 |
| Approaching a surface | Streaming under a residency budget is M6 |
| Budget exceeded | Streaming under a residency budget is M6 |

*Texture edited while running* and *Reload failure keeps the old asset* were on that list until M3:
both are `integration.assets_watch` now — the first as a watcher poll followed by a `reload()` that
the pre-existing `Ref` sees, the second as a reload whose read fails while the old bytes stay in
use.

Two scenarios have a case in **two** suites, because they have two halves that are tested in
different places: *Platform variants* (the key is a value in `test_identity.cpp`; the addressing is
in `test_package.cpp`), *Source deleted* (the reserved placeholder id in `test_identity.cpp`; the
load that serves it in `test_asset_system.cpp`) and *Partial read of a large asset* (the codec in
`test_compression.cpp`; end to end, counting bytes off disk, in `test_package.cpp`).

| Suite | Kind | Sources |
|---|---|---|
| `unit.assets` | unit | `test_identity.cpp`, `test_path.cpp`, `test_serialization.cpp` |
| `integration.assets_codec` | integration | `test_compression.cpp` |
| `integration.assets_io` | integration | `test_file.cpp`, `test_package.cpp`, `test_vfs.cpp` |
| `integration.assets_loading` | integration | `test_asset_system.cpp` |

The suites are declared through a **deferred call** because `cy_add_test()` is defined by
`tests/CMakeLists.txt`, which the top level adds *after* `src/`. `src/core/reflect/`,
`src/core/values/` and `src/core/jobs/` each hit this independently and solved it the same way; this
is the fourth copy of one workaround, and whoever closes M1 should settle it — either move
`add_subdirectory(tests)` above `add_subdirectory(src)` at the top level, or lift the test taxonomy
into a `cmake/` module both sides include.

## M11.d: what M11.d added here, and the row read requirement by requirement

`core-assets-and-io` is claimed **Complete** at M11.d. Its two named absences were closed here and
the whole row was then read at Complete grade — requirement by requirement, satisfied / partial /
unmet, the way M10 read `save-and-persistence`. A row nobody read is a row nothing checks.

### The two absences, closed

**1. "Development file serving" has a transport** (`remote.h`, `remote.cpp`,
`tests/test_remote.cpp`). M1 wrote `RemoteFileProvider` as a seam and said why it stopped there —
*"a protocol nobody has spoken is a protocol that is wrong"* — and for ten milestones the only
implementation in the tree was `FakeHost` in `test_vfs.cpp`. There is now a host
(`FileServingHost`) and a client (`SocketFileProvider`) speaking a three-operation protocol over
TCP, and `integration.assets_remote` runs **both ends over the loopback**: a device mounts a
`RemoteMount`, reads two files, and the fetch counters show it fetched each one when it was read and
not before — the scenario's own words, asserted rather than described.

Three properties are worth reading before the code: a path arriving over the socket is **re-run
through `VirtualPath::normalise` by the host**, because the traversal rule is the type's and a
hand-written client is exactly what it exists to refuse; the host **binds the loopback unless asked
otherwise**, because a development file server that publishes a project directory to the network by
default is a decision nobody made; and the socket half is **POSIX, compiled on Linux and macOS**,
with Windows refusing rather than pretending, exactly as `net::UdpTransport` does.

**2. `AssetSystem::reload` no longer refuses a package-backed asset.** The old refusal named three
things — chunk framing, decompression, the dependency list — and all three are `PackageReader`'s, so
the reload path uses the load pipeline rather than reimplementing it. `integration.assets_watch`
covers a re-cooked package mounted over the one a resident asset came from: the `Ref` a material
would be holding reads the new bytes, the newly declared dependency is started, and a second reload
does not start it twice. The specification's hot-reload requirement names *cooked outputs* as well
as source files, so the refusal was the requirement unmet rather than scoped.

### The row at Complete grade

| Requirement | Verdict | Evidence, and what is missing |
|---|---|---|
| Asset identity | **satisfied** | `identity.cpp`; `unit.assets` covers the moved asset, the duplicated `.meta` and the reserved placeholder id |
| Assets are cooked, not parsed at runtime | **satisfied** | `cooked.h` header carries magic, version, kind, content hash and variant key; `integration.assets_io` covers the version mismatch and the variant addressing |
| Virtual filesystem | **satisfied** — closed here | five mount kinds, priority resolution, patch masking, traversal refused by `VirtualPath`. "Development file serving" was the last unimplemented row and is above |
| Package format | **partial** | directory, chunks, manifest, dedup, seekable framing, mapping and deleted-entry markers all exist and are tested. **The two encryption flags are declared and refused on open** (`package_reader.cpp`): a package that sets them cannot be read, so "package flags SHALL support encrypted directory, encrypted payload" is a declaration rather than a capability |
| Asset loading | **satisfied** | `load`, `load_async`, `load_batch`, `preload`/`release`, `Ref<T>`, three retention policies, and the four stages on the async service and job workers; `integration.assets_loading`, 30 cases |
| Streaming | **satisfied** | `streaming.h`: mip/LOD/audio ladders under a residency budget, a not-yet-resident level falls back to the highest resident one; `integration.assets_streaming` |
| Hot reload | **satisfied** — closed here | `watch.h` fingerprints and debounces; `reload` swaps in place preserving every `Ref`, over a loose file **and over a package** |
| File and directory access | **partial** | read, write, seek, size, flush, mapping, directory enumeration and atomic rename-based writes all exist — **behind `CY_ASSETS_POSIX_IO`**. The non-POSIX branch returns `Unsupported`, so on Windows this requirement is unimplemented rather than untested, and no CI leg has ever run it |
| Serialization formats | **satisfied, within the vocabulary reflection emits** | binary and text round-trip with stable field order; `reflect::FieldKind` covers scalars, enumerations and flag sets, and a field outside that is reported by name rather than dropped. Strings, containers and nested structs are `core-type-system`'s vocabulary, not this module's omission |
| Compression and cryptography | **UNMET IN PART, and it is the largest hole in this row** | `core/compression` promises LZ4, Zstd and Deflate: **only Zstd is implemented**, and `CompressionMethod::Lz4` and `::Deflate` are enumerators with a comment saying no codec is pinned. `core/crypto` promises SHA-256, BLAKE3, HMAC, AES-GCM and a CSPRNG: **only BLAKE3 exists** (`hash.h` says so in its own header comment). The content-hash and tampered-package scenarios pass because both run on BLAKE3; nothing else in the requirement has an implementation |

**Two findings this audit produced that are not in any requirement.**

*A dead second copy of this module's watcher is still in the tree, and it redefines a public class.*
`include/cy/core/assets/hot_reload.h`, `src/hot_reload.cpp` and `tests/test_hot_reload.cpp` are an
abandoned M2-era generation of `watch.h`: **`src/hot_reload.cpp` is in no `SOURCES` list and
`test_hot_reload.cpp` is in no suite**, so neither is compiled by anything. The header is the
problem rather than the sources: it is on this module's **public** include path and it declares a
second, differently-shaped `cy::assets::FileWatcher`, `FileChange`, `FileWatcherStats` and
`file_change_name` in the same namespace as `watch.h`'s. Any consumer that included it would link
against `watch.cpp`'s definitions with the dead header's layout, which is an ODR violation the
compiler cannot see. It is not a live bug: the only file that includes it,
`src/backends/shader/include/cy/shader/hot_reload.h`, is itself part of a **dead duplicate header
tree** under `src/backends/shader/include/cy/shader/` that no compiled source reaches (`shader.cpp`
and `spirv.cpp` are in no `SOURCES` list either). Both trees want deleting in one change — the
assets half alone would leave a dangling include in the shader half — and the deletion also removes
one line from `tools/quality/licence_baseline.txt`. This module's own `CMakeLists.txt` warns about
exactly this shape in its header comment: *"a file this list does not name is compiled by nothing,
which reads in a review exactly like a file that passes."*

*`Mount::contains()` returns `bool`, so a mount that cannot answer is indistinguishable from one
that says no.* Measured through the new transport: when the host process goes away, the provider
reports `Unavailable` and counts a `transport_failure`, and one layer up
`VirtualFileSystem::resolve` reports **`NotFound`** — a dead host reads as a missing file, which is
the diagnostic that sends a developer to look at their content. `test_remote.cpp` asserts both
levels, including the wrong-looking one, because that is what a caller sees today. The fix is an
interface change every mount pays for (`contains` returning `Expected<bool, Error>`, or a `probe`
beside it) and it belongs with whoever next opens `vfs.h`.
