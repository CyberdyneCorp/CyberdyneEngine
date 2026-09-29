// SPDX-License-Identifier: MIT
// `lightmap` as a graph node: a level's bake description and its imported meshes in, one cooked
// lightmap out. `rendering-global-illumination` — "Lightmap baking": "Baking SHALL be incremental
// where possible: unchanged geometry and lighting SHALL reuse previous results."
//
// ================================================================================================
// THE CONTENT KEY IS THE INCREMENTALITY, AT LEVEL GRANULARITY
// ================================================================================================
//
// The node's key is the description's digest, the upstream import nodes' OUTPUT digests and the
// producer version — `build-and-packaging`'s ordinary derivation. So an unchanged level, lights,
// sky, settings and meshes all the same, is served from the artefact store and never re-baked,
// and a changed light or a moved object is a new key and a new bake. What this does NOT do is
// re-solve only the region one moved object affects: that is the incremental rebake #36's next
// slice owns, and the key here is exactly what it will refine.
//
// ================================================================================================
// THE DESCRIPTION
// ================================================================================================
//
//     cylightmap 1
//     mode directional              irradiance | directional | sh-l1
//     bounces 2
//     samples 32
//     density 4                     texels per metre
//     page 256                      texels per page side
//     seed 12345
//     sky 0.2 0.25 0.3              a uniform sky radiance, or:
//     sky 0.3 0.4 0.6 0.5 0.5 0.5 0.1 0.1 0.1   zenith, horizon, ground
//     light directional <dx dy dz> <intensity> <r g b> [static | stationary | movable]
//     light point <px py pz> <intensity> <range> <r g b> [static | stationary | movable]
//
// A light with no mobility word is stationary (`gi::LightMobility`'s default).
//     material "white" <r g b> [emission <r g b>] [opacity <a>]
//     instance "derived/room.bundle" "mesh/Room" "white" <scale> <12 floats, 3x4 row-major>
//     instance ".cy/cooked/<id>.cyasset" "mesh" "white" <scale> <12 floats>
//
// A `.cyasset` path is one cooked mesh as `cy_import_cli` writes it into a project, its sub-asset
// name unread: what the editor's bake command reads, outside the graph.
// Every bundle an instance names must be the output of a declared upstream, which is what makes an
// edited mesh invalidate the bake.

#include "lightmap_producer.h"

#include "text.h"

#include <cy/core/assets/cooked.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/gltf.h>
#include <cy/import/pipeline.h>
#include <cy/rendering/lightmap_bake/asset.h>
#include <cy/rendering/lightmap_bake/bake.h>

#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cy::build {
namespace {

namespace bake = rendering::lightmap_bake;
namespace gi = rendering::gi;

[[nodiscard]] f32 number(const text::Line& line, usize index) {
    return std::strtof(std::string(line.word(index)).c_str(), nullptr);
}

[[nodiscard]] Vec3 triple(const text::Line& line, usize index) {
    return Vec3{number(line, index), number(line, index + 1), number(line, index + 2)};
}

/// A mesh read out of an import bundle, owning its arrays.
struct LoadedMesh {
    import::MeshData data;
    bake::BakeMesh mesh;
};

/// Everything the description names, owned for the length of the bake.
struct LevelDescription {
    bake::LightmapBakeSettings settings;
    gi::SkyTerm sky{};
    std::vector<gi::GiLight> lights;
    std::vector<bake::BakeMaterial> materials;
    std::map<std::string, u32, std::less<>> material_names;
    std::deque<LoadedMesh> meshes;
    std::map<std::string, u32, std::less<>> mesh_names;
    std::vector<bake::BakeInstance> instances;
    std::vector<bake::BakeMesh> mesh_views;

    [[nodiscard]] bake::LightmapScene scene() {
        mesh_views.clear();
        for (const LoadedMesh& loaded : meshes) {
            mesh_views.push_back(loaded.mesh);
        }
        bake::LightmapScene out;
        out.meshes = {mesh_views.data(), mesh_views.size()};
        out.materials = {materials.data(), materials.size()};
        out.instances = {instances.data(), instances.size()};
        out.lights = {lights.data(), lights.size()};
        out.sky = sky;
        return out;
    }
};

[[nodiscard]] Status parse_setting(const text::Line& line, LevelDescription& level) {
    const std::string_view key = line.word(0);
    if (key == "mode") {
        const std::string_view mode = line.word(1);
        for (u32 index = 0; index < static_cast<u32>(bake::LightmapMode::Count); ++index) {
            if (mode == bake::lightmap_mode_name(static_cast<bake::LightmapMode>(index))) {
                level.settings.mode = static_cast<bake::LightmapMode>(index);
                return ok();
            }
        }
        return fail(ErrorCode::InvalidArgument, "a lightmap description names an unknown mode");
    }
    const auto count = [&line]() {
        return static_cast<u32>(std::strtoul(std::string(line.word(1)).c_str(), nullptr, 10));
    };
    if (key == "bounces") {
        level.settings.trace.bounces = count();
    } else if (key == "samples") {
        level.settings.trace.samples = count();
    } else if (key == "seed") {
        level.settings.trace.seed = count();
    } else if (key == "page") {
        level.settings.atlas.page_size = count();
    } else if (key == "density") {
        level.settings.atlas.texel_density = number(line, 1);
    } else if (key == "sky") {
        const bool uniform = line.words.size() == 4;
        level.sky.zenith = triple(line, 1);
        level.sky.horizon = uniform ? level.sky.zenith : triple(line, 4);
        level.sky.ground = uniform ? level.sky.zenith : triple(line, 7);
    } else {
        return fail(ErrorCode::InvalidArgument, "a lightmap description line nothing reads");
    }
    return ok();
}

/// The optional mobility word after a light's colour. Unknown words are an error rather than
/// ignored, so a misspelt `stationery` does not silently bake as the default.
[[nodiscard]] Status parse_mobility(std::string_view word, gi::LightMobility& out) {
    if (word.empty() || word == "stationary") {
        out = gi::LightMobility::Stationary;
    } else if (word == "static") {
        out = gi::LightMobility::Static;
    } else if (word == "movable") {
        out = gi::LightMobility::Movable;
    } else {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap light's mobility is not static, stationary or movable");
    }
    return ok();
}

[[nodiscard]] Status parse_light(const text::Line& line, LevelDescription& level) {
    gi::GiLight light;
    light.id = level.lights.size() + 1U;
    usize mobility_at = 0;
    if (line.word(1) == "directional") {
        light.directional = true;
        light.direction = triple(line, 2);
        light.intensity = number(line, 5);
        light.colour = triple(line, 6);
        mobility_at = 9;
    } else {
        light.position = triple(line, 2);
        light.intensity = number(line, 5);
        light.range = number(line, 6);
        light.colour = triple(line, 7);
        mobility_at = 10;
    }
    if (Status parsed = parse_mobility(line.word(mobility_at), light.mobility); !parsed) {
        return parsed;
    }
    level.lights.push_back(light);
    return ok();
}

void parse_material(const text::Line& line, LevelDescription& level) {
    bake::BakeMaterial material;
    material.albedo = triple(line, 2);
    for (usize at = 5; at < line.words.size(); ++at) {
        if (line.word(at) == "emission" && at + 3U < line.words.size()) {
            material.emission = triple(line, at + 1U);
            at += 3U;
        } else if (line.word(at) == "opacity" && at + 1U < line.words.size()) {
            material.opacity = number(line, at + 1U);
            at += 1U;
        }
    }
    level.material_names.emplace(std::string(line.word(1)),
                                 static_cast<u32>(level.materials.size()));
    level.materials.push_back(material);
}

/// Point a loaded mesh's bake view at its arrays and measure its unwrap.
void view_mesh(LoadedMesh& loaded) {
    const import::MeshData& data = loaded.data;
    loaded.mesh.positions = {data.positions.data(), data.positions.size()};
    loaded.mesh.normals = {data.normals.data(), data.normals.size()};
    loaded.mesh.uv0 = {data.uvs.data(), data.uvs.size()};
    loaded.mesh.uv2 = {data.uv2.data(), data.uv2.size()};
    loaded.mesh.indices = {data.indices.data(), data.indices.size()};
    bake::measure_uv2(loaded.mesh.uv2, loaded.mesh.indices, loaded.mesh.uv_coverage,
                      loaded.mesh.uv_aspect);
}

/// One cooked mesh out of a `.cyasset` file — what `cy_import_cli` writes into a project's
/// `.cy/cooked/`, one file per sub-asset.
[[nodiscard]] Status read_cooked_asset(Span<const u8> bytes, import::MeshData& out) {
    const Expected<Span<const u8>, Error> payload =
        assets::read_cooked_payload(bytes.data(), bytes.size(), true);
    if (!payload.has_value()) {
        return make_unexpected(payload.error());
    }
    return import::read_cooked_mesh(payload.value(), out);
}

/// The mesh in an upstream import bundle's sub-asset `name`.
[[nodiscard]] Status read_bundle_mesh(Span<const u8> bytes, std::string_view name,
                                      import::MeshData& out) {
    import::ImportResult result;
    if (Status decoded = import::decode_import_bundle(bytes, result); !decoded) {
        return decoded;
    }
    for (const import::SubAsset& asset : result.assets()) {
        if (asset.view() == name) {
            return import::read_cooked_mesh(
                Span<const u8>(asset.payload.data(), asset.payload.size()), out);
        }
    }
    return fail(ErrorCode::NotFound, "a lightmap instance names a sub-asset its bundle lacks");
}

/// The mesh an instance names, read once per (bundle, sub-asset): out of an upstream import bundle,
/// or — for a path ending `.cyasset`, whose sub-asset name is not read — out of a cooked asset file.
[[nodiscard]] Expected<u32, Error> mesh_for(const BundleSource& source, std::string_view bundle,
                                            std::string_view name, LevelDescription& level) {
    std::string key(bundle);
    key += '\n';
    key += name;
    if (const auto found = level.mesh_names.find(key); found != level.mesh_names.end()) {
        return found->second;
    }
    Array<u8> bytes(default_allocator());
    if (Status read = source.read(source.user, bundle, bytes); !read) {
        return make_unexpected(read.error());
    }
    LoadedMesh& loaded = level.meshes.emplace_back();
    const Span<const u8> view(bytes.data(), bytes.size());
    const Status parsed = bundle.ends_with(".cyasset") ? read_cooked_asset(view, loaded.data)
                                                       : read_bundle_mesh(view, name, loaded.data);
    if (!parsed) {
        level.meshes.pop_back();
        return make_unexpected(parsed.error());
    }
    view_mesh(loaded);
    const auto index = static_cast<u32>(level.meshes.size() - 1U);
    level.mesh_names.emplace(std::move(key), index);
    return index;
}

[[nodiscard]] Status parse_instance(const BundleSource& source, const text::Line& line,
                                    LevelDescription& level) {
    if (line.words.size() != 17U) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap instance is a bundle, a mesh, a material, a scale and 12 numbers");
    }
    const auto material = level.material_names.find(line.word(3));
    if (material == level.material_names.end()) {
        return fail(ErrorCode::InvalidArgument, "a lightmap instance names an undeclared material");
    }
    Expected<u32, Error> mesh = mesh_for(source, line.word(1), line.word(2), level);
    if (!mesh) {
        return make_unexpected(mesh.error());
    }
    bake::BakeInstance instance;
    instance.mesh = mesh.value();
    instance.material = material->second;
    instance.resolution_scale = number(line, 4);
    f32 rows[12] = {};
    for (u32 index = 0; index < 12U; ++index) {
        rows[index] = number(line, 5U + index);
    }
    instance.transform = Mat4::from_columns(
        Vec4{rows[0], rows[4], rows[8], 0.0F}, Vec4{rows[1], rows[5], rows[9], 0.0F},
        Vec4{rows[2], rows[6], rows[10], 0.0F}, Vec4{rows[3], rows[7], rows[11], 1.0F});
    instance.id = level.instances.size();
    level.instances.push_back(instance);
    return ok();
}

[[nodiscard]] Status parse(const BundleSource& source, std::string_view document,
                           LevelDescription& level) {
    auto lines = text::read(document);
    if (!lines) {
        return make_unexpected(lines.error());
    }
    if (lines->empty() || lines->front().word(0) != "cylightmap" || lines->front().word(1) != "1") {
        return fail(ErrorCode::InvalidArgument, "not a `cylightmap 1` description");
    }
    for (usize index = 1; index < lines->size(); ++index) {
        const text::Line& line = (*lines)[index];
        const std::string_view key = line.word(0);
        if (key == "light") {
            if (Status parsed = parse_light(line, level); !parsed) {
                return parsed;
            }
        } else if (key == "material") {
            parse_material(line, level);
        } else if (key == "instance") {
            if (Status parsed = parse_instance(source, line, level); !parsed) {
                return parsed;
            }
        } else if (Status parsed = parse_setting(line, level); !parsed) {
            return parsed;
        }
    }
    return ok();
}

[[nodiscard]] Status read_upstream(void* user, std::string_view name, Array<u8>& out) {
    return static_cast<NodeContext*>(user)->read(name, out);
}

}  // namespace

Status bake_lightmap_description(std::string_view document, const BundleSource& source,
                                 const rendering::lightmap_bake::LightmapBakeProgress* progress,
                                 Array<u8>& payload, LightmapJobReport& report) {
    auto level = std::make_unique<LevelDescription>();
    if (Status parsed = parse(source, document, *level); !parsed) {
        report.stage = "lightmap-description";
        return parsed;
    }
    bake::BakedLightmap baked;
    if (Status made = bake::bake_lightmaps(level->scene(), level->settings, nullptr, baked,
                                           report.bake, progress);
        !made) {
        report.stage = "lightmap-bake";
        return made;
    }
    report.device_bytes = baked.device_bytes() + baked.shadow_mask_bytes();
    report.page_size = baked.page_size;
    report.mip_levels = baked.mip_levels;
    return bake::encode_lightmap_asset(baked, payload);
}

Status produce_lightmap(NodeContext& context) {
    const NodeDesc& node = context.node();
    if (node.sources.size() != 1 || node.outputs.size() != 1) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap node declares exactly one description and one output");
    }
    Array<u8> bytes(default_allocator());
    if (Status read = context.read(node.sources.front(), bytes); !read) {
        return read;
    }
    const BundleSource upstream{&read_upstream, &context};
    Array<u8> payload(default_allocator());
    LightmapJobReport report;
    if (Status baked = bake_lightmap_description(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), upstream,
            nullptr, payload, report);
        !baked) {
        context.diagnose(Severity::Error, report.stage, baked.error().message,
                         node.sources.front());
        return baked;
    }
    return context.write(node.outputs.front(), payload.data(), payload.size());
}

}  // namespace cy::build
