# `tools/` — layer 7

Command-line utilities and code generators. Tools are built when `CY_BUILD_TOOLS` is on and are
never linked into a shipped runtime.

**What belongs here**: the reflection generator, the ABI and overlay generators, the layering
checker, the dependency-manifest and attribution generators, the roadmap tooling, the cooker, the
packager and the shader compiler driver.

**What does not belong here**: anything the runtime links. A tool may depend on any layer; nothing
may depend on a tool.

**Governed by**: `build-system-and-platforms` (code generation, distribution artefacts),
`build-and-packaging`, `asset-import-pipeline`, `delivery-roadmap`.
