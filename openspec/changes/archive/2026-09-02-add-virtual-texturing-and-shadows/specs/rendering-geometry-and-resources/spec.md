## MODIFIED Requirements

### Requirement: Texture streaming
Textures SHALL declare a **residency model** (see `virtual-texturing`): fully resident,
mip-streamed, virtual streamed, or virtual runtime.

**Mip streaming** — partial residency by mip level — SHALL remain a first-class model for ordinary
assets, driven by renderer feedback and by distance-based heuristics where feedback is unavailable.
It is simpler and cheaper than virtual texturing for the majority of a project's textures and SHALL
NOT be treated as a legacy path.

Mip streaming SHALL: hold a residency budget from the memory budget tree, prioritise through the
shared residency policy (see `residency`), prefetch on visibility prediction, and never block the
frame — a non-resident mip SHALL fall back to the highest resident one.

The lowest few mips SHALL always be resident so no texture is ever entirely missing.

Virtual residency models SHALL be governed by `virtual-texturing` rather than by this requirement.

#### Scenario: Approach a surface
- **WHEN** the camera approaches and higher mips are sampled
- **THEN** they SHALL be scheduled and swapped in when ready, with the lower mip shown meanwhile

#### Scenario: Budget pressure
- **WHEN** the residency budget is exceeded
- **THEN** eviction SHALL follow the shared residency policy rather than a texture-specific rule,
  and the eviction SHALL be reported

#### Scenario: Model is chosen per asset
- **WHEN** a small user-interface texture and a terrain material are cooked
- **THEN** the first SHALL be fully resident and the second virtual, from their declared models
