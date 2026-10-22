# Tasks

- [x] Write the `.cymap` source and its strict parser (`map_source.*`), the package's assets and payloads (`map_format.*`), and the kit (`map_meshes.*`).
- [x] Write the cook (`map_cook.*`, `cy_rts_map_cook`): the terrain through `terrain::evaluate_region`, the albedo bake, the forest through `foliage::generate_region`, the kit, the package; cook it in the build.
- [x] Write the loader (`map_load.*`): the package mounted, every asset read by id through `AssetSystem`, kind, hash and format checked.
- [x] Draw the session on the map (`host/map_render.*`, `--map`): the standard material and its textures, shadows, AO, the GI with the map's boxes, blue's fog and the minimap's overlay, the HUD, the skinned units.
- [x] Reuse the sky's irradiance in `FrameAssembly` while its inputs hold; append `AssemblyReport::sky_irradiance_reused`.
- [x] Add `integration.rts_map`, `smoke.rts_api_map` and the assembly case; prove each red by a mutation and record it in `evidence/`.
- [x] Add `just capture-rts-map`; publish the images and the video.
- [x] Update the sample's README, `content/rts/`, `deps/shipped-content.md`, `THIRD_PARTY.md`, `docs/guides/rts-map.md`, the requirements map and this change, and validate it.
