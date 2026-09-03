# `src/` — the engine

The C++20 engine, organised as the layer stack `engine-architecture` fixes. A lower layer never
depends on a higher one; the rule is enforced at configure time by `cy_add_module()` and at source
level by `just quality-layers`, not by review.

| Directory | Layer | Responsibility |
|---|---:|---|
| `core/` | 0 | Type system, memory, containers, math, jobs, assets, platform abstraction, diagnostics |
| `ecs/` | 1 | Archetype world, components, queries, system scheduling |
| `servers/` | 2 | Handle-based services: render, physics, audio, navigation, text, display, input |
| `backends/` | 3 | Concrete server implementations: Vulkan, Metal, Jolt, platform audio |
| `scene/` | 4 | Node façade, transforms, prefabs, built-in components and node types |
| `runtime/` | 5 | Engine bootstrap, subsystem wiring, `Runtime::tick()` |
| `abi/` | 6 | The stable flat C ABI exported to scripting and extensions |

`platform/` (layer 3) and `editor/`, `tools/` (layer 7) sit outside this tree; see their own READMEs.

**Governed by**: `engine-architecture`.
