// SPDX-License-Identifier: MIT
#ifndef CY_BUILD_SRC_LIGHTMAP_PRODUCER_H
#define CY_BUILD_SRC_LIGHTMAP_PRODUCER_H
// The `lightmap` producer, and the same bake outside the graph for `cy_build lightmap` — the
// editor's bake command. Private to tools/build/src/; `add_content_producers` registers the
// producer.

#include <cy/build/producer.h>
#include <cy/rendering/lightmap_bake/bake.h>

#include <string_view>

namespace cy::build {

/// A `cylightmap 1` description and its upstream mesh bundles in, one cooked lightmap
/// (`lightmap_bake::encode_lightmap_asset`) out.
[[nodiscard]] Status produce_lightmap(NodeContext& context);

/// Where a description's mesh bundles are read from: the graph's upstream artefacts for the
/// producer, the project's files for the command line.
struct BundleSource {
    Status (*read)(void* user, std::string_view name, Array<u8>& out) = nullptr;
    void* user = nullptr;
};

/// What one bake outside the graph did.
struct LightmapJobReport {
    rendering::lightmap_bake::LightmapBakeReport bake;
    /// The diagnostic code of the step that failed: "lightmap-description" or "lightmap-bake".
    const char* stage = "";
    u64 device_bytes = 0;
    u32 page_size = 0;
    u32 mip_levels = 0;
};

/// Bake a description: parse it, read its bundles through `source`, bake with `progress` (which
/// may be null, and may cancel), and encode the cooked payload. The same bytes the producer writes
/// for the same description and bundles.
[[nodiscard]] Status bake_lightmap_description(
    std::string_view document, const BundleSource& source,
    const rendering::lightmap_bake::LightmapBakeProgress* progress, Array<u8>& payload,
    LightmapJobReport& report);

}  // namespace cy::build

#endif  // CY_BUILD_SRC_LIGHTMAP_PRODUCER_H
