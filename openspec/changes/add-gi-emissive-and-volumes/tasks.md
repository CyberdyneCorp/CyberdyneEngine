# Tasks

- [x] `GiLight::emitter_area` / `emitter_normal`, `emitter_basis`, `emitter_form_factor`, `shadow_target`; `direct_radiance` and `shaded_direct` integrate a promoted card.
- [x] `gi/emissive.h`: `classify_emitters`, `EmissiveLights`, `EmissiveReport`, `emitter_influence_radius`.
- [x] `IlluminationSystem::ingest_cell`, `evict_cell`, `frame_lights`, `emitters`; `IlluminationSettings::emissive`.
- [x] `PathTracer::set_emission_footprints`.
- [x] The device: `giEmitterFormFactor`, the emitter shadow target, and the light record's fourth `float4`; regenerate the SPIR-V and MSL headers.
- [x] `gi/gi_volume.h`: `GiVolume`, `GiVolumeSet`, `validate_gi_volume`.
- [x] Probe placement, scheduling, gather and the tiered tracer read the volumes; `indirect_diffuse` / `indirect_specular` apply intensity and far field; `IlluminationSystem::set_volumes`.
- [x] Suites: `render_gi_emissive`, `render_gi_volumes`, the resolution cases in `render_gi`, the device emitter case in `render.gi_gpu`.
- [x] Prove each new case red by a mutation (`evidence/falsification.txt`).
- [x] READMEs, `docs/guides/global-illumination.md`, `requirements-coverage.toml`.
- [ ] Promoted emitters in the forward frame's clustered lights (needs a runtime owner of the GI system; issue #35 stage 4).
- [ ] GI volumes from the editor and from Swift.
