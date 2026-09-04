# `src/rendering/shaders/` — layer 4

The engine's own Slang source: the **shader standard library** and the shaders the frame is built
from.

**Governed by**: `shader-system`, which requires the engine to provide "a shader standard library of
Slang modules: BRDF functions, light evaluation, cluster lookup, shadow sampling, GI sampling,
tonemapping, colour space conversion, packing and encoding helpers, noise, and sampling patterns".

This directory compiles no C++. `cy::shaders` is an INTERFACE target whose build step stages the
tree into `${CMAKE_BINARY_DIR}/shaders/`, so a test, a tool or the sample mounts that directory
through `cy::assets::VirtualFileSystem` and reaches the modules by the same virtual paths a shipped
build will. The compiler that consumes them is `src/backends/shader/` at layer 3 — a SPIR-V header
may not appear above the backends.

## The modules

| Module | What it holds |
|---|---|
| `cy/brdf.slang` | GGX, Smith height-correlated visibility, Schlick Fresnel, Lambert |
| `cy/light.slang` | punctual light evaluation in physical units, camera-relative |
| `cy/cluster.slang` | the cluster grid and its exponential depth slicing |
| `cy/shadow.slang` | shadow sampling, with the sample count as a specialization constant |
| `cy/material.slang` | the material interface: one method, returning a `Surface` |
| `cy/tonemap.slang` | Reinhard and the ACES filmic approximation, with exposure separated |
| `cy/color.slang` | the exact sRGB transfer function, and Rec. 709 luminance |
| `cy/packing.slang` | octahedral normal encoding, and the unorm inverses Slang does not ship |
| `cy/sampling.slang` | Hammersley, cosine hemisphere, GGX importance sampling, a branchless basis |
| `cy/noise.slang` | integer hashes and value noise — never a `sin`-based hash |
| `cy/globals.slang` | the global parameter block at set 0, binding 0 |
| `cy/view.slang` | the per-view constants at set 1, in camera-relative space |
| `cy/fullscreen.slang` | the full-screen triangle and a tonemapping resolve over it |

## Three conventions these files keep

**A module, never a textual include.** `shader-system`: shared shader code "SHALL live in a Slang
module imported by each, with no textual preprocessor inclusion". Every file above is a `module` and
every dependency is an `import`.

**Camera-relative, from the first draw** (design.md §3). `cy/view.slang` has no world-to-clip matrix
and `cy/light.slang`'s `Light` has no world-space position. Retrofitting this when world partition
puts real content at real distances means revisiting every shader and every transform path that
assumed world space.

**Reversed-Z is a number.** The depth buffer is `[0,1]`, cleared to 0, compared `GreaterEqual`.
`cy/fullscreen.slang` writes 1 for a pass that must survive the test, and `cy/shadow.slang` compares
in the same direction. Neither is a comment about the convention; both are the convention.

## The material interface

A material implements one method and returns a `Surface`. It cannot see a light, a cluster, a shadow
atlas or a render target, so the engine keeps control of the light loop and the pass structure —
which is what lets the pipeline change without every material changing with it. `IMaterial` is a
Slang *interface* rather than a preprocessor hook, so two materials become two specialisations of
one shading function rather than two textual copies of it.

M7's material compiler generates a `struct` implementing that interface and hands it to
`cy::shader::SourceRegistry::add_generated()`. That is the extension point, and it is deliberately
this small.
