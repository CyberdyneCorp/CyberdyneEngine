# src/rendering/lights_gpu/ — many lights and area lights, on the device

`cy::rendering-lights-gpu`. The device half of the frame's lighting at scale: the LTC table area
lights are shaded with, the cluster lists assigned by a compute pass, and the stochastic many-light
path. The host half — the records, the area shading's twin, the reservoir algebra and the path
selection — is `src/rendering/lighting/`; the shading itself is `cy/light.slang` and
`cy/frame.slang`. docs/guides/many-lights.md is the guide.

![Four hundred coloured lights on the stochastic path](../../../docs/design/images/many-lights-stochastic.png)

| File | What it is |
|---|---|
| `ltc_texture.h` | `LtcTableTexture`: `LtcTable::build`'s 24×24 fit as a 24×48 `Rgba32Float` texture (the inverse matrix above, magnitude and Fresnel below), uploaded once into the frame's texture table. `pack_ltc_texels` is the layout, for a test |
| `lighting_control.h` | `write_ltc_slot` and `write_stochastic_lighting`: the view block's `lighting_control` words |
| `light_assignment.h` | `GpuLightAssignment`: one dispatch a frame. Each workgroup takes a tile, keeps the lights whose bounding sphere reaches the tile's frustum (and a spot's cone, its cluster), and writes each depth slice's nearest 64 into the headers and indices the frame reads; the overflow is counted (`AssignmentStatistics`). The light records are copied to device-local memory first (`lights()`), because the forward pass reads them per fragment |
| `many_lights.h` | `ManyLightsPass`: candidates from an alias table over the lights' power, RIS into a reservoir, temporal reuse with a capped history, spatial reuse with balance-heuristic weights, one screen-space visibility ray, and the denoiser (`accumulate`, variance, à-trous) transcribed from `denoise::Denoiser`'s `StochasticDirect`. `ManyLightsSettings`; `ManyLightsReadback` for a test; `report()` for the path's statistics |
| `shaders/light_assignment.slang`, `shaders/many_lights.slang` | the dispatches; `regenerate.py` writes `src/lights_gpu_spirv.h` and `src/lights_gpu_msl.h` |

## Where it sits in the frame

`AssemblyDescription::device_light_assignment` makes the frame read the device's lists:
`FrameSinks::cluster_assignment` is the dispatch, declared before the prepass, and
`FrameUpload::cluster_headers_buffer`, `cluster_indices_buffer` and `lights_buffer` bind its
buffers. `FramePassKind::ManyLights` (`FrameFeatures::many_lights`, which derives the depth prepass's
normals) runs after the prepass; the forward pass reads its denoised irradiance and its reservoirs
from the texture table (`lighting_control.z`, `.w`) and shades each pixel's selected light's
specular from the reservoir. `FrameAssembly::set_many_lights` switches path from the next frame; the
choice is `rendering::LightingPathSelector`'s. Off, nothing is declared and the frame is the frame
before this module.

## Measured (RTX 5060, Vulkan, `render.many_lights` and `render.area_lights`)

| | |
|---|---|
| 1000 lights, 1920×1080: device assignment | 0.19 ms (budget 0.5) |
| 1000 lights: prepass end to opaque end | 0.70 ms (budget 1.5) |
| stochastic, no reuse: bias against the exact sum | −0.16% |
| stochastic, temporal and spatial reuse: bias | +0.66% (bound 1%) |
| denoiser against `Denoiser::denoise` | worst 0.09% |
| converged: denoised relative deviation, frame change | 1.05% (raw 26.8%), 0.24% |
| clustered against stochastic, open floor | −3.82% mean, 14.6% mean absolute (bounds 5%, 20%) |
| area lights: device against `shade_area_light` | worst 0.30% of 43 124 floor pixels |

## What is not here

Ray-traced visibility: the stochastic path's visibility is one screen-space ray against the depth,
so an occluder off screen does not shadow. The device paths run on Vulkan; Metal and D3D12 compile
both dispatches and were not run.
