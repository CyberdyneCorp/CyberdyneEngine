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
// Outside the graph, `lightmap_level_key` computes the same derivation over the description and
// every file it read, and `cy_build lightmap` keeps it beside its output: the editor rewrites a
// level's description on every bake request, and an unchanged world is not baked again.
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
//     light directional <dx dy dz> <intensity> <r g b> [static | stationary | movable] [id <n>]
//     light point <px py pz> <intensity> <range> <r g b> [static | stationary | movable] [id <n>]
//
// A light with no mobility word is stationary (`gi::LightMobility`'s default). `id` is the scene
// light's stable identity (`gi::GiLight::id`, which the cooked lightmap's shadow-mask channels and
// directly baked lights name); without it a light is numbered from one in order.
//     material "white" <r g b> [emission <r g b>] [opacity <a>]
//     material "wall" cooked ".cy/cooked/<id>.cyasset" [tint <r g b>]
//
// The second form is the material a placed object draws with, as `cy_import_cli` cooked it: its
// base colour, times the renderer's tint, is the albedo and its emissive colour the emission, so
// the bake bounces the colour the frame draws.
//     instance "derived/room.bundle" "mesh/Room" "white" <scale> <12 floats, 3x4 row-major>
//     instance ".cy/cooked/<id>.cyasset" "mesh" "white" <scale> <12 floats> [id <n>] [occluder]
//     instance "meshes/floor.cyprim" "mesh" "white" <scale> <12 floats> [id <n>]
//
// A `.cyasset` path is one cooked mesh as `cy_import_cli` writes it into a project, its sub-asset
// name unread: what the editor's bake command reads, outside the graph. A `.cyprim` is a primitive
// source, generated and unwrapped here exactly as the importer would. Every bundle an instance
// names must be the output of a declared upstream, which is what makes an edited mesh invalidate
// the bake. `id` is the scene object's stable identity (`BakeInstance::id`); without it an
// instance is numbered from zero in order. `occluder`: the object shadows and bounces light but
// owns no lightmap.
//     volume <id> <ox oy oz> <spacing> <cx cy cz> <rays>
//
// An irradiance volume: the scene object's identity, the world position of probe (0, 0, 0), the
// metres between probes, the probes along x, y and z — the extent is `spacing * (count - 1)` —
// and capture rays per probe. Captured after the atlas with the bake's own tracer
// (`capture_irradiance_volumes`) into the probe payload (`lightmap_bake/probes.h`).
//
// Every addition since #36 — `id`, `occluder`, `cooked` materials, `.cyprim` instances and
// `volume` lines — is optional, so a description written before them reads as it did.

#include "lightmap_producer.h"

#include "text.h"

#include <cy/build/content_producers.h>
#include <cy/build/key.h>
#include <cy/build/toolchain.h>
#include <cy/core/assets/cooked.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/gltf.h>
#include <cy/import/model.h>
#include <cy/import/pipeline.h>
#include <cy/import/primitive.h>
#include <cy/rendering/lightmap_bake/asset.h>
#include <cy/rendering/lightmap_bake/probes.h>

#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cy::build {
namespace {

namespace bake = rendering::lightmap_bake;
namespace gi = rendering::gi;

/// Probes one volume may have: a grid past this is a typo, not a level.
constexpr u64 kMaxVolumeProbes = 65536;

[[nodiscard]] f32 number(const text::Line& line, usize index) {
    return std::strtof(std::string(line.word(index)).c_str(), nullptr);
}

[[nodiscard]] Vec3 triple(const text::Line& line, usize index) {
    return Vec3{number(line, index), number(line, index + 1), number(line, index + 2)};
}

[[nodiscard]] u32 count(const text::Line& line, usize index) {
    return static_cast<u32>(std::strtoul(std::string(line.word(index)).c_str(), nullptr, 10));
}

/// A stable identity: an unsigned decimal, all of it. Zero and trailing junk are refused.
[[nodiscard]] Expected<u64, Error> identity(std::string_view word) {
    const std::string text(word);
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (text.empty() || end != text.c_str() + text.size() || value == 0U) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap description's id is not a nonzero number");
    }
    return static_cast<u64>(value);
}

/// Read one file through `source`, recording its content as an input of the level's key.
[[nodiscard]] Status read_input(const BundleSource& source, std::string_view name,
                                LightmapLevel& level, Array<u8>& out) {
    if (Status read = source.read(source.user, name, out); !read) {
        return read;
    }
    level.inputs.push_back(
        LightmapInput{std::string(name), assets::content_hash(out.data(), out.size())});
    return ok();
}

[[nodiscard]] Status parse_setting(const text::Line& line, LightmapLevel& level) {
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
    if (key == "bounces") {
        level.settings.trace.bounces = count(line, 1);
    } else if (key == "samples") {
        level.settings.trace.samples = count(line, 1);
    } else if (key == "seed") {
        level.settings.trace.seed = count(line, 1);
    } else if (key == "page") {
        level.settings.atlas.page_size = count(line, 1);
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

/// A light's mobility word. Unknown words are an error rather than ignored, so a misspelt
/// `stationery` does not silently bake as the default.
[[nodiscard]] Status parse_mobility(std::string_view word, gi::LightMobility& out) {
    if (word == "stationary") {
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

/// The words after a light's colour: an optional mobility word and an optional `id <n>`.
[[nodiscard]] Status parse_light_tail(const text::Line& line, usize at, gi::GiLight& light) {
    bool mobility_seen = false;
    for (; at < line.words.size(); ++at) {
        if (line.word(at) == "id") {
            Expected<u64, Error> id = identity(line.word(at + 1U));
            if (!id) {
                return make_unexpected(id.error());
            }
            light.id = *id;
            ++at;
            continue;
        }
        if (mobility_seen) {
            return fail(ErrorCode::InvalidArgument, "a lightmap light names two mobilities");
        }
        if (Status parsed = parse_mobility(line.word(at), light.mobility); !parsed) {
            return parsed;
        }
        mobility_seen = true;
    }
    return ok();
}

[[nodiscard]] Status parse_light(const text::Line& line, LightmapLevel& level) {
    gi::GiLight light;
    light.id = level.lights.size() + 1U;
    usize tail = 0;
    if (line.word(1) == "directional") {
        light.directional = true;
        light.direction = triple(line, 2);
        light.intensity = number(line, 5);
        light.colour = triple(line, 6);
        tail = 9;
    } else {
        light.position = triple(line, 2);
        light.intensity = number(line, 5);
        light.range = number(line, 6);
        light.colour = triple(line, 7);
        tail = 10;
    }
    if (Status parsed = parse_light_tail(line, tail, light); !parsed) {
        return parsed;
    }
    level.lights.push_back(light);
    return ok();
}

/// `material "name" cooked "<path>"`: the albedo and emission of a cooked material.
[[nodiscard]] Status read_cooked_material(const BundleSource& source, std::string_view path,
                                          LightmapLevel& level, bake::BakeMaterial& out) {
    Array<u8> bytes(default_allocator());
    if (Status read = read_input(source, path, level, bytes); !read) {
        return read;
    }
    const Expected<Span<const u8>, Error> payload =
        assets::read_cooked_payload(bytes.data(), bytes.size(), true);
    if (!payload.has_value()) {
        return make_unexpected(payload.error());
    }
    import::StandardMaterial material;
    if (Status parsed = import::read_cooked_material(payload.value(), material); !parsed) {
        return parsed;
    }
    out.albedo = Vec3{material.base_colour[0], material.base_colour[1], material.base_colour[2]};
    out.emission = Vec3{material.emissive[0], material.emissive[1], material.emissive[2]};
    // A blended material lets light through in proportion to its alpha, as the frame draws it.
    constexpr u32 kBlended = 2;
    if (material.alpha_mode == kBlended) {
        out.opacity = material.base_colour[3];
    }
    return ok();
}

[[nodiscard]] Status parse_material(const BundleSource& source, const text::Line& line,
                                    LightmapLevel& level) {
    bake::BakeMaterial material;
    if (line.word(2) == "cooked") {
        const bool tinted = line.words.size() == 8U && line.word(4) == "tint";
        if (line.words.size() != 4U && !tinted) {
            return fail(ErrorCode::InvalidArgument,
                        "a cooked lightmap material is a name, `cooked`, one path and optionally "
                        "`tint <r g b>`");
        }
        if (Status read = read_cooked_material(source, line.word(3), level, material); !read) {
            return read;
        }
        if (tinted) {
            material.albedo = cwise_mul(material.albedo, triple(line, 5));
        }
    } else {
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
    }
    level.material_names.emplace(std::string(line.word(1)),
                                 static_cast<u32>(level.materials.size()));
    level.materials.push_back(material);
    return ok();
}

/// Point a loaded mesh's bake view at its arrays and measure its unwrap.
void view_mesh(LoadedLightmapMesh& loaded) {
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

/// A `.cyprim` primitive source, generated and unwrapped with the importer's own defaults — what
/// the primitive importer does with `generate-lightmap-uvs` on.
[[nodiscard]] Status read_primitive(Span<const u8> bytes, import::MeshData& out) {
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    Expected<import::PrimitiveSpec, Error> spec = import::parse_primitive_source(text);
    if (!spec) {
        return make_unexpected(spec.error());
    }
    if (Status built = import::build_primitive_mesh(*spec, out); !built) {
        return built;
    }
    const Expected<import::Uv2Report, Error> unwrapped =
        import::generate_uv2_cached(out, import::Uv2Options{}, &import::Uv2Cache::process());
    if (!unwrapped) {
        return make_unexpected(unwrapped.error());
    }
    return ok();
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

[[nodiscard]] Status read_mesh(Span<const u8> bytes, std::string_view bundle, std::string_view name,
                               import::MeshData& out) {
    if (bundle.ends_with(".cyasset")) {
        return read_cooked_asset(bytes, out);
    }
    if (bundle.ends_with(import::kPrimitiveExtension)) {
        return read_primitive(bytes, out);
    }
    return read_bundle_mesh(bytes, name, out);
}

/// The mesh an instance names, read once per (bundle, sub-asset): out of an upstream import bundle,
/// out of a cooked asset file, or generated from a primitive source.
[[nodiscard]] Expected<u32, Error> mesh_for(const BundleSource& source, std::string_view bundle,
                                            std::string_view name, LightmapLevel& level) {
    std::string key(bundle);
    key += '\n';
    key += name;
    if (const auto found = level.mesh_names.find(key); found != level.mesh_names.end()) {
        return found->second;
    }
    Array<u8> bytes(default_allocator());
    if (Status read = read_input(source, bundle, level, bytes); !read) {
        return make_unexpected(read.error());
    }
    LoadedLightmapMesh& loaded = level.meshes.emplace_back();
    if (Status parsed =
            read_mesh(Span<const u8>(bytes.data(), bytes.size()), bundle, name, loaded.data);
        !parsed) {
        level.meshes.pop_back();
        return make_unexpected(parsed.error());
    }
    view_mesh(loaded);
    const auto index = static_cast<u32>(level.meshes.size() - 1U);
    level.mesh_names.emplace(std::move(key), index);
    return index;
}

/// The words after an instance's transform: an optional `id <n>` and an optional `occluder`.
[[nodiscard]] Status parse_instance_tail(const text::Line& line, bake::BakeInstance& instance) {
    constexpr usize kTail = 17;
    for (usize at = kTail; at < line.words.size(); ++at) {
        if (line.word(at) == "occluder") {
            instance.receives_lightmap = false;
            continue;
        }
        if (line.word(at) != "id") {
            return fail(ErrorCode::InvalidArgument,
                        "a lightmap instance is a bundle, a mesh, a material, a scale and 12 "
                        "numbers, then optionally `id <n>` and `occluder`");
        }
        Expected<u64, Error> id = identity(line.word(at + 1U));
        if (!id) {
            return make_unexpected(id.error());
        }
        instance.id = *id;
        ++at;
    }
    return ok();
}

[[nodiscard]] Status parse_instance(const BundleSource& source, const text::Line& line,
                                    LightmapLevel& level) {
    if (line.words.size() < 17U) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap instance is a bundle, a mesh, a material, a scale and 12 numbers");
    }
    bake::BakeInstance instance;
    instance.id = level.instances.size();
    if (Status tail = parse_instance_tail(line, instance); !tail) {
        return tail;
    }
    const auto material = level.material_names.find(line.word(3));
    if (material == level.material_names.end()) {
        return fail(ErrorCode::InvalidArgument, "a lightmap instance names an undeclared material");
    }
    Expected<u32, Error> mesh = mesh_for(source, line.word(1), line.word(2), level);
    if (!mesh) {
        return make_unexpected(mesh.error());
    }
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
    level.instances.push_back(instance);
    return ok();
}

/// `volume <id> <ox oy oz> <spacing> <cx cy cz> <rays>`.
[[nodiscard]] Status parse_volume(const text::Line& line, LightmapLevel& level) {
    if (line.words.size() != 10U) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap volume is an id, an origin, a spacing, three counts and a ray "
                    "count");
    }
    Expected<u64, Error> id = identity(line.word(1));
    if (!id) {
        return make_unexpected(id.error());
    }
    LightmapVolume volume;
    volume.id = *id;
    volume.settings.origin = triple(line, 2);
    volume.settings.spacing_metres = number(line, 5);
    volume.settings.count_x = count(line, 6);
    volume.settings.count_y = count(line, 7);
    volume.settings.count_z = count(line, 8);
    volume.settings.rays_per_probe = count(line, 9);
    const u64 probes =
        u64{volume.settings.count_x} * volume.settings.count_y * volume.settings.count_z;
    if (!(volume.settings.spacing_metres > 0.0F) || probes == 0U || probes > kMaxVolumeProbes ||
        volume.settings.rays_per_probe == 0U) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap volume needs a positive spacing, at least one probe per axis, at "
                    "most 65536 probes and at least one ray");
    }
    level.volumes.push_back(volume);
    return ok();
}

[[nodiscard]] Status parse_line(const BundleSource& source, const text::Line& line,
                                LightmapLevel& level) {
    const std::string_view key = line.word(0);
    if (key == "light") {
        return parse_light(line, level);
    }
    if (key == "material") {
        return parse_material(source, line, level);
    }
    if (key == "instance") {
        return parse_instance(source, line, level);
    }
    if (key == "volume") {
        return parse_volume(line, level);
    }
    return parse_setting(line, level);
}

[[nodiscard]] Status read_upstream(void* user, std::string_view name, Array<u8>& out) {
    return static_cast<NodeContext*>(user)->read(name, out);
}

/// Capture the level's volumes with the bake's tracer and encode them.
[[nodiscard]] Status capture_volumes(LightmapLevel& level,
                                     const bake::LightmapBakeProgress* progress,
                                     LightmapBakeOutput& out, LightmapJobReport& report) {
    if (level.volumes.empty()) {
        // An empty stage, reported so a progress reader sees the bake reach its end.
        if (progress != nullptr) {
            progress->step(bake::LightmapBakeStage::Probes, 0, 0);
        }
        return bake::encode_probe_asset({}, out.probes);
    }
    std::vector<std::unique_ptr<gi::IrradianceVolume>> volumes;
    std::vector<gi::IrradianceVolume*> pointers;
    std::vector<bake::ProbeVolumeSource> sources;
    for (const LightmapVolume& described : level.volumes) {
        auto& volume = volumes.emplace_back(std::make_unique<gi::IrradianceVolume>());
        if (Status configured = volume->configure(described.settings); !configured) {
            return configured;
        }
        pointers.push_back(volume.get());
        sources.push_back(bake::ProbeVolumeSource{described.id, volume.get()});
        report.probes += volume->probe_count();
    }
    u64 rays = 0;
    if (Status captured = bake::capture_irradiance_volumes(level.scene(), level.settings,
                                                           {pointers.data(), pointers.size()}, rays,
                                                           report.bake, progress);
        !captured) {
        return captured;
    }
    report.bake.rays += rays;
    report.volumes = static_cast<u32>(volumes.size());
    return bake::encode_probe_asset({sources.data(), sources.size()}, out.probes);
}

}  // namespace

bake::LightmapScene LightmapLevel::scene() {
    mesh_views.clear();
    for (const LoadedLightmapMesh& loaded : meshes) {
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

Status read_lightmap_description(std::string_view document, const BundleSource& source,
                                 LightmapLevel& level) {
    auto lines = text::read(document);
    if (!lines) {
        return make_unexpected(lines.error());
    }
    if (lines->empty() || lines->front().word(0) != "cylightmap" || lines->front().word(1) != "1") {
        return fail(ErrorCode::InvalidArgument, "not a `cylightmap 1` description");
    }
    for (usize index = 1; index < lines->size(); ++index) {
        if (Status parsed = parse_line(source, (*lines)[index], level); !parsed) {
            return parsed;
        }
    }
    return ok();
}

Expected<assets::DerivationKey, Error> lightmap_level_key(std::string_view document,
                                                          const LightmapLevel& level) {
    NodeDesc node;
    node.kind = NodeKind::Cook;
    node.name = "lightmap";
    node.producer = "lightmap";
    node.producer_version = kLightmapProducerVersion;
    node.sources = {"description"};
    node.outputs = {"lightmap", "probes"};
    KeyInputs inputs;
    inputs.node = &node;
    inputs.toolchain = &current_toolchain();
    inputs.sources.push_back(
        KeyedDigest{"description", assets::content_hash(document.data(), document.size())});
    for (const LightmapInput& input : level.inputs) {
        inputs.upstreams.push_back(KeyedDigest{input.name, input.hash});
    }
    return derivation_key(inputs);
}

Status bake_lightmap_level(LightmapLevel& level, const bake::LightmapBakeProgress* progress,
                           LightmapBakeOutput& out, LightmapJobReport& report) {
    bake::BakedLightmap baked;
    if (Status made = bake::bake_lightmaps(level.scene(), level.settings, nullptr, baked,
                                           report.bake, progress);
        !made) {
        report.stage = "lightmap-bake";
        return made;
    }
    report.device_bytes = baked.device_bytes() + baked.shadow_mask_bytes();
    report.page_size = baked.page_size;
    report.mip_levels = baked.mip_levels;
    if (Status captured = capture_volumes(level, progress, out, report); !captured) {
        report.stage = "lightmap-probes";
        return captured;
    }
    return bake::encode_lightmap_asset(baked, out.payload);
}

Status bake_lightmap_description(std::string_view document, const BundleSource& source,
                                 const bake::LightmapBakeProgress* progress,
                                 LightmapBakeOutput& out, LightmapJobReport& report) {
    auto level = std::make_unique<LightmapLevel>();
    if (Status parsed = read_lightmap_description(document, source, *level); !parsed) {
        report.stage = "lightmap-description";
        return parsed;
    }
    return bake_lightmap_level(*level, progress, out, report);
}

Status produce_lightmap(NodeContext& context) {
    const NodeDesc& node = context.node();
    if (node.sources.size() != 1 || node.outputs.empty() || node.outputs.size() > 2) {
        return fail(ErrorCode::InvalidArgument,
                    "a lightmap node declares exactly one description, one lightmap output and "
                    "at most one probe output");
    }
    Array<u8> bytes(default_allocator());
    if (Status read = context.read(node.sources.front(), bytes); !read) {
        return read;
    }
    const BundleSource upstream{&read_upstream, &context};
    LightmapBakeOutput out;
    LightmapJobReport report;
    if (Status baked = bake_lightmap_description(
            std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), upstream,
            nullptr, out, report);
        !baked) {
        context.diagnose(Severity::Error, report.stage, baked.error().message,
                         node.sources.front());
        return baked;
    }
    if (report.volumes > 0U && node.outputs.size() < 2U) {
        const char* message =
            "a lightmap description with volumes needs a node with a probe output";
        context.diagnose(Severity::Error, "lightmap-probes", message, node.sources.front());
        return fail(ErrorCode::InvalidArgument, message);
    }
    if (Status written =
            context.write(node.outputs.front(), out.payload.data(), out.payload.size());
        !written) {
        return written;
    }
    if (node.outputs.size() < 2U) {
        return ok();
    }
    return context.write(node.outputs[1], out.probes.data(), out.probes.size());
}

}  // namespace cy::build
