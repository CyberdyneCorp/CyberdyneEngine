# Design

## GTAO, full resolution, normals from the prepass

The horizon search is Jimenez et al.'s ground-truth ambient occlusion: `slices` image-plane
whole-pixel lattice directions per pixel (so every tap reconstructs exactly in its slice plane),
rotated by a 4x4 ordered dither, a quadratic march of `steps` depth taps each
way out to the radius, the highest horizon per side, and the closed-form cosine-weighted arc
integral against the normal projected into the slice. SSAO was rejected because its answer is a
sample ratio that must be tuned per scene and darkens open surfaces.

The sum over slices is divided by the same slices' unoccluded integral rather than by the slice
count, and every sample's horizon cosine is biased down by 0.03. Together they make an open plane
exactly unoccluded rather than unoccluded on average, which is what lets the frame test hold open
floor to one 8-bit step.

Normals come from the `DepthNormal` prepass the frame already derives when ambient occlusion is on.
Reconstruction from depth was rejected: it is wrong at silhouettes and contacts, where occlusion is
decided. The term is computed at full resolution; `resolution_scale` is not read.

The bent normal is the visibility-weighted mean of each slice's mid-horizon direction, written
camera-relative in the target's rgb with the visibility in alpha.

## The filter is the shared denoiser's spatial stage

`denoise::Denoiser` is host code over spans. `gtao_filter.slang` transcribes `filter_pixel` and
`tap_weight`, and each pass is configured from `default_config(SignalKind::AmbientOcclusion)` and
the quality ladder's first position: three passes at steps 1, 2 and 4, a 5x5 B3-spline kernel, and
edge-stopping by relative depth, normal power and the visibility value at `sigma_value` spatial
standard deviations. The variance is the denoiser's spatial estimate for a pixel with one sample,
recomputed from the raw term in each pass rather than stored. The device suite runs the device's raw
term through `Denoiser::denoise` and compares.

There is no temporal accumulation: the dither is fixed in screen space, so a still view is still. Under
temporal anti-aliasing's jitter the term follows the moving depth; the device suite bounds what that
adds to the resolved frame over occluded pixels the frame without the stage leaves still, and the
bound is one that removing the cascade exceeds. Reseeding the search's noise every frame does not
exceed it: the cascade removes that noise, which is the argument for keeping no history.

## Where it sits in the frame

`FrameStageDeclaration` lets the ambient occlusion stage be several passes. The frame calls the
producer at the stage's position with the depth, the normal target and the stage's target; the
producer declares one search pass and the cascade, the last writing the target, and returns the
first pass or refuses. The target is persistent and imported (as `Undefined`, since every texel is
rewritten) because the forward pass samples it through a texture-table slot, which names a view.

The forward fragment applies `ambient *= visibility` only when `occlusion_control.x` names a slot,
so a frame that sets nothing shades exactly as before. With the setting off no stage is declared.

## Backends

Vulkan is built and tested. Metal's MSL is generated and committed, with the depth bound as
`DepthTexture2D` so Metal sees `depth2d`; it is compiled by Slang only on this host. D3D12 is not
supported: both entries compile to DXIL, but no DXIL is embedded and the D3D12 descriptor mapping of
the compute set is unverified.
