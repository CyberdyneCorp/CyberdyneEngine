# `src/core/assets` — asset identity, the virtual filesystem, packages, and loading

Layer 0, target `cy::core-assets`, headers `<cy/core/assets/*.h>`, namespace `cy::assets`.
Section 3.3 of `openspec/changes/implement-m1-substrate/tasks.md`, governed by `core-assets-and-io`.

**This capability reaches SEED at M1, not Working.** What is here is identity, the virtual
filesystem, the package format's read path, asynchronous loading, file and directory access, the two
serialization forms, and compression. What is deliberately absent is listed under *Seams* below.

## The map

| Header | What it owns |
|---|---|
| `path.h` | `VirtualPath` — normalised, case-sensitive, traversal-proof, fixed capacity |
| `hash.h` | `ContentHash`, `ContentHasher` — BLAKE3 behind an engine-owned type |
| `identity.h` | `AssetKind`, `VariantKey`, `AssetMeta` and its sidecar text form, `AssetDatabase`, placeholders |
| `compression.h` | `CompressionMethod`, block and **seekable framed** compression, the recompression policy |
| `file.h` | `File`, `MappedFile`, `fs::` directory operations, `fs::write_atomic` |
| `vfs.h` | `Mount`, `DirectoryMount`, `MemoryMount`, `RemoteMount`, `VirtualFileSystem` |
| `package.h` | The `.cypak` format: `PackageReader`, `PackageWriter`, `PackageSet`, `PackageMount` |
| `serialization.h` | The binary envelope and the text form over reflected data |
| `asset_system.h` | `AssetSystem`, `AssetData`, `LoadRequestId`, the retention policies |
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
| Streaming, per-mip residency, renderer feedback | `AssetSystem` retention is about *when memory goes back*, not about partial residency | M6 |
| GPU upload | `AssetSystemStats::uploads_skipped` counts the stage that is skipped | M3 |
| Hot reload | Not in tasks 3.3.1–3.3.6; the `Mount` interface is where a watcher would sit | M2+ |
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
| Texture edited while running | Hot reload; not in tasks 3.3.1–3.3.6 |
| Reload failure keeps the old asset | Hot reload; not in tasks 3.3.1–3.3.6 |

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
