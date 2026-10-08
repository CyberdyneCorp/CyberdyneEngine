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
| [Deterministic math](deterministic-math.md) | `cy::core-detmath` (issue #89): `Fixed`, `Fixed16`, `Angle` and `WideFixed`; the arithmetic rules; the transcendentals and their declared bounds; the float boundary; what linking the module changes in the determinism profiles; the golden vectors, the oracle, the general-registers and AVX2 builds and the cross-leg digest; regeneration, pitfalls and what is not built yet |
| [Physics in CyberEngine](physics.md) | How physics is layered over Jolt; authoring bodies in C++, scene files and the editor; the fixed step, interpolation and determinism; queries from C++ and Swift; characters, joints, ragdolls and buoyancy; the suites and the pitfalls |
| [Lighting authoring and lightmap baking](lighting.md) | The lighting editor's volumes, light mobility and lightmap resolution; how the editor writes the level's `.cylightmap` from the world and `cy_build lightmap` bakes it and its irradiance volumes; caching, cancel and the suites |
| [Navigation authoring](navigation.md) | The editor's navigation baking tool (issue #28): what the engine bakes and what the editor requests; baking, checking for a stale bake and testing a path from the Navigation panel or over MCP; the acceptance ledger |
| [Text and fonts](text.md) | The text stack (issue #86): `TextServer` and its two backends; FreeType, HarfBuzz, msdfgen and ICU behind `TextBackend`; faces, fallback chains and variable instances; shaping and bidirectional lines; the three atlases and distance fields; importing and cooking fonts; text in the interface; diagnostics, options, the suites and what is not built yet |
| [Runtime UI](ui.md) | CyberUI on screen (issue #91): the element store, layout and flattening; text in the interface font with the built-in fallback; the interface pass after the tone curve; the developer console; a strategy HUD driven from game code; the suites and what is not built yet |
| [Visual scripting](visual-scripting.md) | Gameplay graphs (issue #29): authoring in the Gameplay Graph panel or over MCP, the engine's compile and its diagnostics on nodes, attaching a graph to an entity and running it in Play, agreeing with a Swift twin float for float, the suites and what is not built yet |

See also the [documentation index](../README.md).
