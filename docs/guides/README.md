# Guides

Task-oriented walkthroughs for contributors. Each guide is a route through the specifications and
module READMEs it links to; where a guide and a specification disagree, the specification in
[`openspec/specs/`](../../openspec/specs/README.md) is the contract.

| Guide | What it covers |
|---|---|
| [Building and running](building.md) | Installing the toolchain, building the engine and the editor, and running them on Linux, macOS and iOS |
| [Slang in CyberEngine](slang.md) | How the engine's shaders are written, compiled to SPIR-V, MSL and DXIL, embedded and tested; the frame block contract; how materials become Slang; the rules and the common errors |
| [Physics in CyberEngine](physics.md) | How physics is layered over Jolt; authoring bodies in C++, scene files and the editor; the fixed step, interpolation and determinism; queries from C++ and Swift; characters, joints, ragdolls and buoyancy; the suites and the pitfalls |

See also the [documentation index](../README.md).
