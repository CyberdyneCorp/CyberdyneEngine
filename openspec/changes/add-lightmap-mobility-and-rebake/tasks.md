# Tasks

- [x] Add `gi::LightMobility` (`Static`, `Stationary` default, `Movable`) and `GiLight::mobility`.
- [x] Bake per light: the direct term for `Static` (and for every non-`Movable` light under `DirectAndIndirect`), `Movable` lights out of the path tracer's list, the shadow-mask plane and its channel ids for `Stationary`, a fifth stationary light refused.
- [x] Carry the mask through the dilation, into `BakedLightmap`, and into the cooked asset (version 2; version 1 still decodes).
- [x] Add `rebake_lightmaps`: the pack comparison and its fallback, the object-granularity region, tracing only the region, seams only inside it, and the byte copy of everything else.
- [x] Add the chart-padding measurement on the raster, `report.padding_short`, and `LightmapBakeSettings::refuse_short_padding`.
- [x] Accept `static | stationary | movable` on a `cylightmap 1` light line.
- [x] Add the cases for every scenario to `integration.render_lightmap_bake`, each proved red by a mutation recorded in `evidence/falsification.txt`.
- [x] Move the shadow-mask, incremental-rebake and chart-padding exemptions in `requirements-coverage.toml` to the new cases, keeping the frame's use of the mask exempt by name.
- [x] Update `src/rendering/lightmap_bake/README.md` and `bake.h`'s "what is not here", and validate this change with `--strict`.
