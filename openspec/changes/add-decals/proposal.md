# Add decals to the device frame

## Why

`rendering-lighting-and-shadows` requires decals: projected oriented boxes writing into albedo,
normal, roughness, metallic and emission BEFORE LIGHTING, "applied through tile or cluster
assignment", participating "in cluster assignment as a distinct element type". The tree had the
decal's CPU half — `lighting/decals.h`'s projection, angle and distance fades, reoriented normal
blend, sort order and budget — and nothing that drew one. The requirements map recorded the
requirement as `exempt:m11e`: "nothing publishes one into the GPU scene, nothing culls one on the
device, and `Decals SHALL participate in cluster assignment as a distinct element type` has no
implementation". The cluster header of type `Decal` existed in every cluster and was empty in every
frame, because nothing handed the assembly a decal.

## What changes

- `AssemblyView::decals`: a view's decals. `FrameAssembly` ranks them in application order
  (`decal_application_order`, ascending sort order, ties on id — now a free function the budget
  shares) and assigns them in the same pass as the lights, as `ClusterElementType::Decal`, each
  bounded by its ORIENTED BOX (`ClusterElement::oriented_box`, a separating-axis test after the
  sphere) and filtered by its own channels. A cluster list names ranks, ascending, so it is walked
  in sort order by construction. `AssemblyReport` gains `decals` and `decal_assignments`.
- `cy::rendering-decals` (`src/rendering/decals/`), a new module: `pack_decal_table` turns a view's
  ranked decals — positions camera-relative, subtracted in double precision — and optionally the
  assembly's decal lists into a table of 32-bit words; `DecalTableTexture` uploads it as an
  `Rgba8Unorm` texture (one word per texel) into the frame's bindless texture table, outside a frame
  or declared into one; `write_decal_frame` names it in the view block.
- `cy/decal.slang`, a new standard-library module: the decal applied to a surface before it is lit —
  the box test, `decal_angle_fade` and `decal_distance_fade` transcribed, authored coverage shapes
  (box, splat, ring, patches) times an optional mask texture, per-channel weights, and the normal
  tilted by the decal's relief through the surface gradient — over any `ICyDecalSource`.
- `cy/frame.slang` appends `decalControl` to the frame block (`FrameViewData` 512 → 528 bytes). With
  a table slot the forward fragment applies the decals of its cluster — read from the cluster
  buffers the light loop reads, at the `Decal` header — to the surface and the shading normal
  before `accumulateLights`, so the decal is lit, shadowed and occluded as its receiver is. Without
  one the frame is unchanged. The committed SPIR-V and MSL are regenerated.
- Two defects found on the way, each with a regression test: `GpuDrawInstance::layer_mask` was
  never written (every draw carried all ones), and `apply_gpu_cull` routed on a survivor's spatial
  flags and then dropped them, so a device-culled draw lost its skinned and shadow-receiver bits.
- `samples/12-beauty` reads `decal-material` and `decal` lines from its `.cyshot` and, with
  `--decals on`, applies them through the same module — scorch marks on the gravel and moss at the
  pillars' feet; `samples/10-world` gains `--ground-marker`, an RTS move marker's ring on the
  terrain where the camera looks.
- Tests: `unit.rendering_decals`, cases in `unit.render_culling`, `unit.render_forward` and
  `integration.render_assembly`, and `render.decals` on a device.

## Scope

The GPU-scene half of the requirement is not built. Decals are assigned by the assembly's CPU
cluster pass (the same pass the lights use, and the reference the specification's compute pass is
to be checked against); nothing publishes a decal into `render::GpuScene` or culls one in a
dispatch, and the budget arbiter's eviction is `DecalBudget`'s, unchanged. A decal's material is a
`decals::DecalMaterial` resolved into its record rather than a slot of the GPU material table, and
the depth prepass's normal target does not see a decal's normal. The requirements map keeps an
exemption for the GPU-scene residency and names each of these.
