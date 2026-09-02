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
