# Guides

Task-oriented walkthroughs for contributors. Each guide is a route through the specifications and
module READMEs it links to; where a guide and a specification disagree, the specification in
[`openspec/specs/`](../../openspec/specs/README.md) is the contract.

| Guide | What it covers |
|---|---|
| [Building and running](building.md) | Installing the toolchain, building the engine and the editor, and running them on Linux, macOS and iOS |
| [Animation in CyberEngine](animation.md) | Skeletons, clips and compiled pose programs; importing a character from FBX; IK and retargeting; GPU skinning and the frame path; ragdoll hand-off; the animated-character sample; testing, pitfalls and what is not built yet |
| [Slang in CyberEngine](slang.md) | How the engine's shaders are written, compiled to SPIR-V, MSL and DXIL, embedded and tested; the frame block contract; how materials become Slang; the rules and the common errors |
| [Swift gameplay in CyberEngine](swift.md) | The Swift layering over the flat C ABI; creating, building and hot-reloading a game module; behaviours, `@Export` and the Inspector; components and systems; the ABI 1.3 game services (time, input, camera, physics, navigation, audio, spawning, selection, logging); the Swift-only RTS sample; adding an ABI entry end to end; testing with `FakeEngine`, determinism, pitfalls and what is not built yet |
| [Physics in CyberEngine](physics.md) | How physics is layered over Jolt; authoring bodies in C++, scene files and the editor; the fixed step, interpolation and determinism; queries from C++ and Swift; characters, joints, ragdolls and buoyancy; the suites and the pitfalls |

See also the [documentation index](../README.md).
