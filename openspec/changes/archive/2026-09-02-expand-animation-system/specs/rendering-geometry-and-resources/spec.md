## MODIFIED Requirements

### Requirement: Skinning
Skinned meshes SHALL be transformed by a **compute pass** writing into per-instance output vertex
buffers, so the result is reusable across passes (depth, shadows, main) without re-skinning.

Bone matrices SHALL be read from the **GPU pose world** (see `animation-and-skinning`), the shared
GPU-side pose representation, rather than from a buffer uploaded independently per consumer.

Skinning SHALL support up to 4 or 8 influences per vertex (selectable per mesh) and **dual
quaternion** skinning as an option alongside linear blend skinning.

Output buffers SHALL be double buffered so the previous frame's positions are available for
motion vectors, with a tolerance so stepped animation still yields correct velocities.

Skinning SHALL respect the instance's animation LOD tier: instances at a baked tier SHALL use pose
textures or vertex animation rather than skeletal skinning, and instances at reduced bone LOD SHALL
use mesh LODs whose influences reference only retained joints.

#### Scenario: Skinned once, used many times
- **WHEN** a skinned mesh appears in the depth prepass, a shadow pass, and the opaque pass
- **THEN** skinning SHALL run once and all three SHALL read the same output buffer

#### Scenario: Skinned bounds
- **WHEN** a skinned mesh animates
- **THEN** its bounds SHALL be computed from bone transforms and per-bone bounds, not from the
  bind pose

#### Scenario: One pose representation
- **WHEN** skinning, motion vector generation, and VFX attachment all need bone transforms
- **THEN** all SHALL read the GPU pose world rather than each maintaining its own upload
