# `tools/gen/` — the code generators

Generated code is a build step with declared inputs and outputs, and generation is **deterministic**:
identical inputs produce byte-identical outputs, so generated artefacts do not churn in builds or
defeat caches. Every generator here is a program rather than a `configure_file()` in CMake, because
a program can be run twice and diffed — and because `--check` is then the same code path as the
write, so a currency check cannot disagree with the generator it checks.

| Generator | Reads | Writes |
|---|---|---|
| `generate_headers.py` | `<build>/generated/cy_generated_inputs.txt`, written by `cmake/features.cmake` | `<build>/generated/include/cy_features.h`, `cy_modules.h` |
| `reflect_gen.py` (`reflect/`) | annotated C++ headers, `identity/manifest.toml`, module attribute schemas | `src/core/reflect/generated/*.reflect.{h,cpp}`, the aggregate registration, and appended manifest entries |

Later milestones add the shader artefacts (M3) and the C ABI headers and Swift overlay (M4) to this
directory.

## The reflection generator

`reflect_gen.py` is a thin entry point over the `reflect/` package: `annotations.py` (the annotation
grammar), `attrspec.py` (the attribute table and module schemas), `parse.py` (libclang),
`manifest.py` (identity), `emit.py` (the generated C++), `cache.py` (incrementality) and `cli.py`.

It parses with a real C++ frontend rather than a text scanner, because `core-type-system` requires
it and because a scanner that is wrong about C++ is wrong silently. Three things about that frontend
are written into `parse.py`'s header comment and are worth knowing before touching it: `cursor.kind`
raises on constructs the pinned bindings do not know, so every comparison is against `_kind_id`;
traversal is a pruned descent rather than `walk_preorder()`, which is both much faster and what keeps
output per header; and the incremental cache is keyed on **content**, never on a modification time.

It needs the libclang Python bindings and a libclang 18 shared library. Neither is required to
*build* the engine — the generated metadata is committed — and `src/core/reflect/CMakeLists.txt`
detects them at configure time and says so.

    pip install --user clang==18.1.8
    sudo apt install libclang1-18          # Debian and Ubuntu

## Recipes

```
just generate-headers     regenerate for a profile's build directory
just generate-check       fail if a generated file is stale, or if regeneration is not reproducible
just generate-test        run the tests below
```

## Where the outputs go

`cy_features.h` and `cy_modules.h` are written to the **build directory**, not to the source tree:
they are a function of the configuration, and two configurations of the same checkout have different
ones.

The reflection metadata and `identity/manifest.toml` are **committed**, for the reasons
`build-system-and-platforms` gives and because the identifiers they carry are a contract: an identity
change has to be reviewable as a diff rather than happening inside a build directory. The Swift
overlay joins them at M4.

## Tests

`tests/run_tests.py` configures real CMake projects against the engine's own `cmake/features.cmake`
and `cmake/modules.cmake`. The fixture projects are written to a temporary directory rather than
committed: what each test is about is the one line that differs from the test above it, and that
reads better here than across a dozen near-identical directories.

It covers the acceptance scenarios of tasks 1.4.4 and 1.5.4 — a disabled feature excludes its
sources, a missing feature dependency fails the configure naming the option, and an unknown module
dependency, a disabled one, a cycle, a layer violation, an undeclared link and a malformed manifest
are each configure errors — plus reproducibility, stale detection, registration ordering, and
out-of-tree module discovery.

`src/core/reflect/tests/test_generator.py` covers the reflection generator: identifier assignment,
the rename and tombstone routes, the gate, reproducibility across build directories, staleness,
incrementality, and every attribute-validation failure. Run it with
`ctest -R integration.reflect_generator` or directly.

**Governed by**: `build-system-and-platforms` (code generation), `project-and-plugins`,
`core-type-system` (the reflection generator).
