# rendering-materials-and-shading Specification

## Purpose

Defines the shading model and material system: the physically-based BRDF the engine implements,
the shading models it supports, how materials are described and instanced, parameter storage,
and texture binding.

The BRDF is specified concretely — exact terms and their sources — because "PBR" alone is not a
specification and mismatched terms produce subtly wrong lighting that is very hard to debug
later.

## Requirements

### Requirement: Core BRDF
Direct lighting for the `Lit` shading model SHALL evaluate a metallic-roughness microfacet BRDF:

**Specular** — Cook-Torrance `D · F · V`:
- **D** (normal distribution): GGX / Trowbridge-Reitz, evaluated in the numerically stable form
  `k = α / (1 − NoH² + α²)`, `D = k² / π`, where `α = roughness²`
- **V** (visibility, Smith height-correlated, already divided by `4·NoL·NoV`):
  `V = 0.5 / lerp(2·NoL·NoV, NoL + NoV, α)` (Hammon's approximation)
- **F** (Fresnel): Schlick, `F = f0 + (f90 − f0)·(1 − VoH)⁵`
- `f0 = lerp(vec3(0.04 · specular²), albedo, metallic)`
- `f90 = saturate(dot(f0, vec3(50/3)))`, providing specular occlusion for very dark `f0`

**Diffuse** — selectable per material:
- `Lambert` (default): `albedo / π`
- `Burley` (Disney): with `FD90 = 0.5 + 2·VoH²·roughness`
- `OrenNayar`: for rough dielectrics where retro-reflection matters

**Energy conservation** — a multi-scatter compensation term SHALL be applied to specular so
rough metals do not lose energy, using the split-sum DFG table's average.

`roughness` SHALL be **perceptual** in material authoring (`α = roughness²`), and SHALL be
clamped to a minimum (default 0.045) to keep highlights representable.

#### Scenario: Metal has no diffuse
- **WHEN** `metallic` is 1.0
- **THEN** the diffuse term SHALL be zero and `f0` SHALL be the albedo

#### Scenario: Rough metal conserves energy
- **WHEN** a metal with roughness 0.9 is lit
- **THEN** multi-scatter compensation SHALL keep its total reflectance close to `f0`, rather than
  darkening as single-scatter GGX would

#### Scenario: Grazing angles
- **WHEN** a very dark dielectric is viewed at a grazing angle
- **THEN** the `f90` term SHALL prevent an unphysically bright rim

### Requirement: Shading models
Shading models SHALL be the **lowered form** of a material's closure set, not the authoring model.
Authors compose closures (see `material-compiler`); the compiler matches the resulting set against
these models and lowers to the matching one.

The engine SHALL implement these models, sharing one light iteration loop:

| Model | Additional terms | Closure set it lowers from |
|---|---|---|
| `Lit` | The core BRDF | diffuse + specular |
| `Unlit` | Emission only; no light iteration | emission |
| `ClearCoat` | A second GGX lobe on the geometric normal with its own roughness and IOR 1.5, attenuating the base layer by `1 − Fc` | diffuse + specular + coat |
| `Anisotropic` | Anisotropic GGX with tangent-space `αx`/`αy` derived from roughness and anisotropy | diffuse + anisotropic specular |
| `SubsurfaceScattering` | Screen-space diffusion plus a transmittance term; see `rendering-post-processing` | diffuse + specular + subsurface |
| `Cloth` | Charlie sheen distribution with an Ashikhmin visibility term, plus optional subsurface colour | sheen + optional subsurface |
| `Hair` | Marschner-style R / TT / TRT lobes approximated for real time | hair specular lobes |
| `Foliage` | Two-sided lighting with a translucency term driven by a thickness map | diffuse + transmission |
| `Water` | Reflection and refraction at a dielectric interface with wavelength-dependent absorption and scattering through the water column, and foam coverage blending toward a diffuse layer | specular + transmission + volumetric absorption |

A closure set matching one of these SHALL cost exactly what that model costs. A closure set
matching none SHALL lower to a **generic layered evaluator**, which SHALL be available on profiles
that declare support for it and SHALL have its higher cost reported at cook time.

Each model SHALL be resolved through a specialization constant.

#### Scenario: Clearcoat attenuates the base
- **WHEN** a clearcoat material is lit
- **THEN** the base layer's contribution SHALL be scaled by `1 − Fc`, so total reflectance stays
  physical

#### Scenario: Anisotropic highlight follows the tangent
- **WHEN** a brushed-metal material has anisotropy 0.8
- **THEN** its highlight SHALL stretch perpendicular to the surface tangent direction

#### Scenario: Generality costs nothing when unused
- **WHEN** a material's closures match the `Lit` model
- **THEN** it SHALL compile to the `Lit` path with no layered-evaluation overhead

#### Scenario: Unmatched closure set
- **WHEN** a material composes closures matching no model
- **THEN** it SHALL lower to the generic evaluator, and its additional cost SHALL be reported

#### Scenario: Water is not opaque PBR
- **WHEN** a water surface is shaded
- **THEN** it SHALL use the `Water` model with absorption over the water column, rather than being
  approximated by a metallic-roughness surface with a tinted colour

### Requirement: Image-based lighting
Indirect specular SHALL use the **split-sum approximation**: a pre-filtered environment map
indexed by roughness, multiplied by a precomputed **DFG** lookup table (a 2D `RG16F` texture
indexed by `NoV` and roughness, generated with GGX importance sampling).

Indirect diffuse SHALL use the environment's irradiance, stored as spherical harmonics (L2) or a
small irradiance map.

Environment maps SHALL be stored as **octahedral** maps rather than cubemaps, giving simpler
filtering, mipmapping, and array storage.

#### Scenario: Roughness selects a mip
- **WHEN** indirect specular is evaluated for roughness 0.5
- **THEN** the corresponding pre-filtered mip SHALL be sampled and scaled by the DFG table's
  `(scale, bias)` for the material's `f0`

#### Scenario: Octahedral seams
- **WHEN** an octahedral map is filtered
- **THEN** border texels SHALL be replicated so bilinear filtering across the octahedral seam is
  correct

### Requirement: Material model
A **material** SHALL consist of: an authored definition (a graph or a text definition), a compiled
**material program** produced by the material compiler, a parameter block, and a resource block.

Parameters SHALL be **static** — part of the program's structure — or **runtime** — data that
changes without recompilation. The classification SHALL be derived by the compiler from how a
parameter is used (see `material-compiler`).

Materials SHALL support **instancing**: a material instance references a program and overrides a
subset of runtime parameters. Instances SHALL be creatable at runtime without compilation, and any
number SHALL share one program.

Blend modes: `Opaque`, `Masked` (alpha test with a threshold), `Translucent`, `Additive`,
`Modulate`, `PremultipliedAlpha`.

#### Scenario: Instance shares a pipeline
- **WHEN** 50 material instances derive from one parent
- **THEN** all SHALL use the same program and pipeline, and sorting SHALL group them together

#### Scenario: Masked material in the prepass
- **WHEN** a `Masked` material is rendered
- **THEN** it SHALL participate in the depth prepass with its alpha test applied, so the opaque
  pass can use `Equal` depth testing

#### Scenario: Runtime parameter change compiles nothing
- **WHEN** gameplay changes a runtime parameter
- **THEN** no compilation SHALL occur and the change SHALL apply the same frame

### Requirement: Parameter storage
Material parameters and resource references SHALL live in the **GPU material table** (see
`material-compiler`), addressed by a material identifier carried in GPU scene instance data.

Textures SHALL be referenced by index into bindless descriptor arrays, with per-material
descriptor sets used only on the compatibility path described in `rhi-and-render-graph`.

Parameter names SHALL resolve to compile-time identifiers; per-frame string lookup SHALL NOT be
required.

Parameter updates SHALL be batched: changed materials SHALL be collected during Prepare and
uploaded in one transfer.

#### Scenario: Parameter change is cheap
- **WHEN** a script changes one material parameter
- **THEN** only that material's range SHALL be marked dirty and included in the next batched
  upload

#### Scenario: Per-instance parameter override
- **WHEN** an instance overrides a material parameter (a tint per entity)
- **THEN** the override SHALL be stored in per-instance data, not by duplicating the material

#### Scenario: Shading reaches parameters without binding
- **WHEN** a GPU-generated draw shades a pixel
- **THEN** it SHALL index the material table using the instance's material identifier, with no
  per-object descriptor binding

### Requirement: Standard material
The engine SHALL provide a **standard material** covering common needs without custom shader
authoring, with slots for: base colour (factor and texture), metallic, roughness, normal,
ambient occlusion, emission, opacity, height (parallax occlusion mapping), clearcoat and
clearcoat roughness, anisotropy and its direction, subsurface colour and thickness, and a detail
layer with its own UV scale and blend mode.

Texture slots SHALL support: per-slot UV channel selection, tiling and offset, and channel
packing (occlusion-roughness-metallic in one texture).

Additional options SHALL include: triplanar mapping, world-space or local-space UVs, vertex
colour modulation, two-sided rendering with normal flipping, and distance and proximity fade.

#### Scenario: Packed ORM texture
- **WHEN** occlusion, roughness, and metallic share one texture
- **THEN** the material SHALL sample it once and route channels, rather than three samples

#### Scenario: Triplanar on terrain
- **WHEN** triplanar mapping is enabled
- **THEN** the material SHALL blend three projections weighted by the world normal, with a
  configurable sharpness

### Requirement: Texture sampling conventions
Colour textures SHALL be stored and sampled in **sRGB**, decoded to linear by hardware. Data
textures (normal, roughness, metallic, masks) SHALL be linear.

Normal maps SHALL use the **OpenGL convention** (+Y up); importers SHALL convert DirectX-convention
maps at import time.

All lighting math SHALL occur in **linear space**; conversion to display space occurs only at
tonemapping.

#### Scenario: Wrong colour space is caught
- **WHEN** a texture assigned to a data slot is marked sRGB
- **THEN** the import or material validation SHALL warn, since it would be decoded incorrectly

#### Scenario: Normal map convention
- **WHEN** a DirectX-convention normal map is imported
- **THEN** its green channel SHALL be inverted at import, so runtime sampling needs no branch

### Requirement: Material validation
The engine SHALL validate materials at cook time and report: parameters declared but unused,
textures bound to non-existent slots, shading model and blend mode combinations that are
unsupported, and materials whose permutation count exceeds a budget.

#### Scenario: Unsupported combination
- **WHEN** a material requests subsurface scattering with an additive blend mode
- **THEN** cooking SHALL fail with an explanation, rather than producing undefined shading

### Requirement: Fallback materials
The engine SHALL provide fallback materials for error states: missing material, failed shader
compilation, and missing texture — each visually distinctive (magenta checkerboard) so problems
are immediately obvious rather than silently black.

#### Scenario: Shader fails to compile
- **WHEN** a material's shader fails at runtime in a development build
- **THEN** the fallback material SHALL be used and the error logged, and the object SHALL remain
  visible and obviously wrong
