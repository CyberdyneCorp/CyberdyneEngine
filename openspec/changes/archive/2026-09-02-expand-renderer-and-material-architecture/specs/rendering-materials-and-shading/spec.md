## MODIFIED Requirements

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
