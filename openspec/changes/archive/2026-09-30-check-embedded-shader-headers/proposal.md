# Proposal

## Why

Passes that must exist in a build without a shader compiler embed their compiled SPIR-V and MSL as
committed headers. Nothing checked that a header still matched its Slang source. On main the
particle and strip renderers' headers were compiled against a `CyFrameData` fourteen fields shorter
than the frame's current layout, and nothing noticed. Several other headers had drifted too:
licence lines added by hand that their embed scripts do not write, and `#line` numbers left behind
after a source edit.

A clang build of main also failed on this host. Clang 22 reports `__COUNTER__` under `-Wpedantic`
as a C2y extension. Every test declaration expands one, in the harness's `CY_TEST_CASE` and inside
doctest's own macros. The diagnostic lands on the test's line, so treating doctest as a system
header does not suppress it.

## What Changes

- `tools/shaders/embedded_headers.toml` declares how every committed embedded-shader header is
  produced: the slangc invocations, the embed command, and the headers deliberately left alone
  (the DXIL header that needs DXC, and three test fixtures pinned to old commits).
- `tools/shaders/embedded_headers.py check` reruns every group in a scratch copy of the tree and
  compares the output byte for byte with the committed headers. It fails when a header is stale,
  when a declared header is missing, or when a header in the tree matches the name patterns but is
  not declared. `regenerate` writes the headers in place.
- `just quality-shader-headers` runs the check's own negative cases, then the check, against the
  dev build's slangc. It is the permanent gate `shader-headers` and runs in the CI `quality` job,
  which uploads the regenerated text when the check fails.
- The stale headers are regenerated. The embed scripts that dropped the licence line now write it.
  `src/vfx/gpu/shaders/embed_msl.py` takes `--title`, so the iOS sample's kernel header can be
  reproduced by the same script.
- The test harness brackets the macros that expand a `__COUNTER__` with a clang-only
  `-Wc2y-extensions` suppression. It applies only on compilers that know the warning, and the
  project's warning flags do not change.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `shader-system`: committed compiled shaders are checked against their sources.
- `build-system-and-platforms`: a warning that a dependency's macro raises at the expansion site is
  suppressed only for that macro's expansion.

## Impact

- New: `tools/shaders/embedded_headers.{py,toml}`, `tools/shaders/tests/test_embedded_headers.py`,
  the `quality-shader-headers` recipe, the `shader-headers` gate and its CI step.
- Regenerated: the particle and strip headers (SPIR-V and MSL), `frame_spirv.h`,
  `selection_spirv.h`, `vfx_support_spirv.h`, `skin_msl.h`, `gpu_conformance_embedded.h` and
  `samples/03-first-light`'s SPIR-V and MSL.
- `tests/harness/include/cy/test/test.h`.
