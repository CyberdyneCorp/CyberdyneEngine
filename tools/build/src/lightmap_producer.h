// SPDX-License-Identifier: MIT
#ifndef CY_BUILD_SRC_LIGHTMAP_PRODUCER_H
#define CY_BUILD_SRC_LIGHTMAP_PRODUCER_H
// The `lightmap` producer, and the same bake outside the graph for `cy_build lightmap` — the
// editor's bake command. Private to tools/build/src/; `add_content_producers` registers the
// producer.

#include <cy/build/producer.h>
#include <cy/core/assets/derivation.h>
#include <cy/core/assets/hash.h>
#include <cy/import/mesh.h>
#include <cy/rendering/gi/irradiance_volume.h>
#include <cy/rendering/lightmap_bake/bake.h>

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace cy::build {

/// A `cylightmap 1` description and its upstream mesh bundles in, one cooked lightmap
/// (`lightmap_bake::encode_lightmap_asset`) out, and — when the node declares a second output —
/// the captured irradiance volumes (`lightmap_bake::encode_probe_asset`).
[[nodiscard]] Status produce_lightmap(NodeContext& context);

/// Where a description's mesh bundles are read from: the graph's upstream artefacts for the
/// producer, the project's files for the command line.
struct BundleSource {
    Status (*read)(void* user, std::string_view name, Array<u8>& out) = nullptr;
    void* user = nullptr;
};

/// A mesh read out of an import bundle or a cooked asset, owning its arrays.
struct LoadedLightmapMesh {
    import::MeshData data;
    rendering::lightmap_bake::BakeMesh mesh;
};

/// One irradiance volume a description names: the scene object's identity and its probe grid.
struct LightmapVolume {
    u64 id = 0;
    rendering::gi::IrradianceVolumeSettings settings{};
};

/// A file a description made the bake read, by the name the description gave it, and its content.
/// With the description's own digest these are what `lightmap_level_key` is computed from.
struct LightmapInput {
    std::string name;
    assets::ContentHash hash;
};

/// Everything a description names, read and owned for the length of the bake.
struct LightmapLevel {
    rendering::lightmap_bake::LightmapBakeSettings settings;
    rendering::gi::SkyTerm sky{};
    std::vector<rendering::gi::GiLight> lights;
    std::vector<rendering::lightmap_bake::BakeMaterial> materials;
    std::map<std::string, u32, std::less<>> material_names;
    std::deque<LoadedLightmapMesh> meshes;
    std::map<std::string, u32, std::less<>> mesh_names;
    std::vector<rendering::lightmap_bake::BakeInstance> instances;
    std::vector<LightmapVolume> volumes;
    std::vector<LightmapInput> inputs;
    std::vector<rendering::lightmap_bake::BakeMesh> mesh_views;

    /// The bake's view of the level. Points into this level; valid until it changes.
    [[nodiscard]] rendering::lightmap_bake::LightmapScene scene();
};

/// Parse a description and read every file it names through `source`, recording each in
/// `level.inputs`.
[[nodiscard]] Status read_lightmap_description(std::string_view document,
                                               const BundleSource& source, LightmapLevel& level);

/// The key a `lightmap` node over this description would have: the build graph's own derivation
/// (`derivation_key`) of the producer and its version, the toolchain, the description's content and
/// the content of every file it read. Equal keys bake to equal bytes, which is what lets
/// `cy_build lightmap` skip a bake of an unchanged level.
[[nodiscard]] Expected<assets::DerivationKey, Error> lightmap_level_key(std::string_view document,
                                                                        const LightmapLevel& level);

/// What one bake outside the graph did.
struct LightmapJobReport {
    rendering::lightmap_bake::LightmapBakeReport bake;
    /// The diagnostic code of the step that failed: "lightmap-description", "lightmap-bake" or
    /// "lightmap-probes".
    const char* stage = "";
    u64 device_bytes = 0;
    u32 page_size = 0;
    u32 mip_levels = 0;
    /// Irradiance volumes captured, and their probes.
    u32 volumes = 0;
    u32 probes = 0;
};

/// What a bake wrote: the cooked lightmap, and the captured volumes — empty when the level has
/// none.
struct LightmapBakeOutput {
    Array<u8> payload;
    Array<u8> probes;
};

/// Bake a level that `read_lightmap_description` read, with `progress` (which may be null, and may
/// cancel), capture its volumes with the same tracer, and encode both.
[[nodiscard]] Status bake_lightmap_level(
    LightmapLevel& level, const rendering::lightmap_bake::LightmapBakeProgress* progress,
    LightmapBakeOutput& out, LightmapJobReport& report);

/// Parse a description, read its files through `source` and bake it: the same bytes the producer
/// writes for the same description and bundles.
[[nodiscard]] Status bake_lightmap_description(
    std::string_view document, const BundleSource& source,
    const rendering::lightmap_bake::LightmapBakeProgress* progress, LightmapBakeOutput& out,
    LightmapJobReport& report);

}  // namespace cy::build

#endif  // CY_BUILD_SRC_LIGHTMAP_PRODUCER_H
