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
//     light directional <dx dy dz> <intensity> <r g b>
//     light point <px py pz> <intensity> <range> <r g b>
//     material "white" <r g b> [emission <r g b>] [opacity <a>]
//     instance "derived/room.bundle" "mesh/Room" "white" <scale> <12 floats, 3x4 row-major>
//
// Every bundle an instance names must be the output of a declared upstream, which is what makes an
// edited mesh invalidate the bake.

#include "lightmap_producer.h"

#include "text.h"

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

void parse_light(const text::Line& line, LevelDescription& level) {
    gi::GiLight light;
    light.id = level.lights.size() + 1U;
    if (line.word(1) == "directional") {
        light.directional = true;
        light.direction = triple(line, 2);
        light.intensity = number(line, 5);
        light.colour = triple(line, 6);
    } else {
        light.position = triple(line, 2);
        light.intensity = number(line, 5);
        light.range = number(line, 6);
        light.colour = triple(line, 7);
    }
    level.lights.push_back(light);
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

/// The mesh an instance names, read once per (bundle, sub-asset) out of the upstream bundle.
[[nodiscard]] Expected<u32, Error> mesh_for(NodeContext& context, std::string_view bundle,
                                            std::string_view name, LevelDescription& level) {
    std::string key(bundle);
    key += '\n';
    key += name;
    if (const auto found = level.mesh_names.find(key); found != level.mesh_names.end()) {
        return found->second;
    }
    Array<u8> bytes(default_allocator());
    if (Status read = context.read(bundle, bytes); !read) {
        return make_unexpected(read.error());
    }
    import::ImportResult result;
    if (Status decoded =
            import::decode_import_bundle(Span<const u8>(bytes.data(), bytes.size()), result);
        !decoded) {
        return make_unexpected(decoded.error());
    }
    for (const import::SubAsset& asset : result.assets()) {
        if (asset.view() != name) {
            continue;
        }
        LoadedMesh& loaded = level.meshes.emplace_back();
        if (Status parsed = import::read_cooked_mesh(
                Span<const u8>(asset.payload.data(), asset.payload.size()), loaded.data);
            !parsed) {
            return make_unexpected(parsed.error());
        }
        const import::MeshData& data = loaded.data;
        loaded.mesh.positions = {data.positions.data(), data.positions.size()};
        loaded.mesh.normals = {data.normals.data(), data.normals.size()};
        loaded.mesh.uv0 = {data.uvs.data(), data.uvs.size()};
        loaded.mesh.uv2 = {data.uv2.data(), data.uv2.size()};
        loaded.mesh.indices = {data.indices.data(), data.indices.size()};
        bake::measure_uv2(loaded.mesh.uv2, loaded.mesh.indices, loaded.mesh.uv_coverage,
                          loaded.mesh.uv_aspect);
        const auto index = static_cast<u32>(level.meshes.size() - 1U);
        level.mesh_names.emplace(std::move(key), index);
        return index;
    }
    return fail(ErrorCode::NotFound, "a lightmap instance names a sub-asset its bundle lacks");
}

[[nodiscard]] Status parse_instance(NodeContext& context, const text::Line& line,
                                    LevelDescription& level) {
    if (line.words.size() != 17U) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap instance is a bundle, a mesh, a material, a scale and 12 numbers");
    }
    const auto material = level.material_names.find(line.word(3));
    if (material == level.material_names.end()) {
        return fail(ErrorCode::InvalidArgument, "a lightmap instance names an undeclared material");
    }
    Expected<u32, Error> mesh = mesh_for(context, line.word(1), line.word(2), level);
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
    instance.transform = Mat4::from_columns(Vec4{rows[0], rows[4], rows[8], 0.0F},
                                            Vec4{rows[1], rows[5], rows[9], 0.0F},
                                            Vec4{rows[2], rows[6], rows[10], 0.0F},
                                            Vec4{rows[3], rows[7], rows[11], 1.0F});
    instance.id = level.instances.size();
    level.instances.push_back(instance);
    return ok();
}

[[nodiscard]] Status parse(NodeContext& context, std::string_view document,
                           LevelDescription& level) {
    auto lines = text::read(document);
    if (!lines) {
        return make_unexpected(lines.error());
    }
    if (lines->empty() || lines->front().word(0) != "cylightmap" ||
        lines->front().word(1) != "1") {
        return fail(ErrorCode::InvalidArgument, "not a `cylightmap 1` description");
    }
    for (usize index = 1; index < lines->size(); ++index) {
        const text::Line& line = (*lines)[index];
        const std::string_view key = line.word(0);
        if (key == "light") {
            parse_light(line, level);
        } else if (key == "material") {
            parse_material(line, level);
        } else if (key == "instance") {
            if (Status parsed = parse_instance(context, line, level); !parsed) {
                return parsed;
            }
        } else if (Status parsed = parse_setting(line, level); !parsed) {
            return parsed;
        }
    }
    return ok();
}

}  // namespace

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
    auto level = std::make_unique<LevelDescription>();
    if (Status parsed =
            parse(context,
                  std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()),
                  *level);
        !parsed) {
        context.diagnose(Severity::Error, "lightmap-description", parsed.error().message,
                         node.sources.front());
        return parsed;
    }
    bake::BakedLightmap baked;
    bake::LightmapBakeReport report;
    if (Status made = bake::bake_lightmaps(level->scene(), level->settings, nullptr, baked, report);
        !made) {
        context.diagnose(Severity::Error, "lightmap-bake", made.error().message,
                         node.sources.front());
        return made;
    }
    Array<u8> payload(default_allocator());
    if (Status encoded = bake::encode_lightmap_asset(baked, payload); !encoded) {
        return encoded;
    }
    return context.write(node.outputs.front(), payload.data(), payload.size());
}

}  // namespace cy::build
