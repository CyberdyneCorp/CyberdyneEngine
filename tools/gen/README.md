# `tools/gen/` — the code generators

Generated code is a build step with declared inputs and outputs, and generation is **deterministic**:
identical inputs produce byte-identical outputs, so generated artefacts do not churn in builds or
defeat caches. Every generator here is a program rather than a `configure_file()` in CMake, because
a program can be run twice and diffed — and because `--check` is then the same code path as the
write, so a currency check cannot disagree with the generator it checks.

| Generator | Reads | Writes |
|---|---|---|
| `generate_headers.py` | `<build>/generated/cy_generated_inputs.txt`, written by `cmake/features.cmake` | `<build>/generated/include/cy_features.h`, `cy_modules.h` |

Later milestones add the reflection metadata (M1), the shader artefacts (M3), the C ABI headers and
the Swift overlay (M4) to this directory.

## Recipes

```
just generate-headers     regenerate for a profile's build directory
just generate-check       fail if a generated file is stale, or if regeneration is not reproducible
just generate-test        run the tests below
```

## Where the outputs go

`cy_features.h` and `cy_modules.h` are written to the **build directory**, not to the source tree:
they are a function of the configuration, and two configurations of the same checkout have different
ones. The Swift overlay and the identity manifest are the two artefacts that will be committed
instead, for the reasons `build-system-and-platforms` gives.

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

**Governed by**: `build-system-and-platforms` (code generation), `project-and-plugins`.
