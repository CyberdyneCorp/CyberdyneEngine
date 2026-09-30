// SPDX-License-Identifier: MIT
// The lightmap bake as a node in the derivation graph. `integration.build_content`.
//
// `rendering-global-illumination` — "Lightmap baking": a cooked, content-keyed lightmap, so that
// "unchanged geometry and lighting SHALL reuse previous results"; and "UV2 and chart packing",
// "Unwrap is cached": a rebuild that changes nothing runs neither the bake nor the unwrap.

#include "lightmap_producer.h"

#include <cy/build/content_producers.h>
#include <cy/build/description.h>
#include <cy/build/service.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/mesh.h>
#include <cy/rendering/lightmap_bake/asset.h>
#include <cy/rendering/lightmap_bake/atlas.h>
#include <cy/rendering/lightmap_bake/probes.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

using namespace cy;
using namespace cy::build;

namespace {

[[nodiscard]] Allocator& test_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

void append_f32(std::string& out, float value) {
    u8 bytes[4] = {};
    std::memcpy(static_cast<void*>(bytes), &value, sizeof(value));
    out.append(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void append_u16(std::string& out, u16 value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

/// A unit quad in the XY plane, as an external glTF buffer.
[[nodiscard]] std::string quad_buffer() {
    std::string buffer;
    const float positions[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    for (const auto& position : positions) {
        append_f32(buffer, position[0]);
        append_f32(buffer, position[1]);
        append_f32(buffer, position[2]);
    }
    for (const u32 index : {0U, 1U, 2U, 2U, 1U, 3U}) {
        append_u16(buffer, static_cast<u16>(index));
    }
    return buffer;
}

[[nodiscard]] std::string quad_document(usize buffer_bytes) {
    std::string document;
    document += R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)";
    document += std::to_string(buffer_bytes);
    document += R"(,"uri":"quad.bin"}],"bufferViews":[)";
    document += R"({"buffer":0,"byteOffset":0,"byteLength":48},)";
    document += R"({"buffer":0,"byteOffset":48,"byteLength":12}],)";
    document += R"("accessors":[)";
    document += R"({"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},)";
    document += R"({"bufferView":1,"componentType":5123,"count":6,"type":"SCALAR"}],)";
    document += R"("meshes":[{"name":"Panel","primitives":[{"attributes":{"POSITION":0},)"
                R"("indices":1,"mode":4}]}],)";
    document += R"("nodes":[{"name":"Quad","mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    return document;
}

constexpr const char* kDescription =
    "cybuild 1\n"
    "node \"import:quad\" import \"import\" 1\n"
    "  source \"assets/quad.gltf\"\n"
    "  output \"derived/quad.bundle\"\n"
    "  option \"variant\" \"desktop\"\n"
    "  option \"opt.generate-lightmap-uvs\" \"true\"\n"
    "node \"lightmap:level\" cook \"lightmap\" 1\n"
    "  source \"levels/level.cylightmap\"\n"
    "  upstream \"import:quad\"\n"
    "  output \"derived/level.lightmap\"\n";

/// A wall of four quads' size standing on a floor quad, lit by one point light. `mobility` is the
/// light line's optional trailing word; empty leaves it out.
[[nodiscard]] std::string level_description(float intensity, std::string_view mobility = {}) {
    std::string text =
        "cylightmap 1\n"
        "mode directional\n"
        "bounces 1\n"
        "samples 8\n"
        "density 4\n"
        "page 128\n"
        "sky 0.2 0.25 0.3\n";
    text += "light point 0 1 1.5 " + std::to_string(intensity) + " 20 1 1 1";
    if (!mobility.empty()) {
        text += " " + std::string(mobility);
    }
    text += "\n";
    text +=
        "material \"white\" 0.7 0.7 0.7\n"
        "material \"red\" 0.8 0.1 0.1\n";
    // A 4 m wall in the XY plane, and the same quad turned to be a floor in front of it.
    text +=
        "instance \"derived/quad.bundle\" \"mesh/Panel\" \"red\" 1 "
        "4 0 0 -2  0 4 0 0  0 0 4 0\n";
    text +=
        "instance \"derived/quad.bundle\" \"mesh/Panel\" \"white\" 1 "
        "4 0 0 -2  0 0 4 0  0 -4 0 4\n";
    return text;
}

class Project {
public:
    explicit Project(const char* label, const char* description = kDescription) : temp_(label) {
        CY_REQUIRE(temp_.valid());
        CY_REQUIRE(producers_.add_builtins().has_value());
        CY_REQUIRE(add_content_producers(producers_, nullptr).has_value());
        CY_REQUIRE(read_description(description, graph_, &producers_).has_value());
        const std::string buffer = quad_buffer();
        write("assets/quad.bin", buffer);
        write("assets/quad.gltf", quad_document(buffer.size()));
        write("levels/level.cylightmap", level_description(20.0F));
    }

    void write(const std::string& name, std::string_view content) const {
        const std::string path = temp_.path() + "/project/" + name;
        const usize slash = path.rfind('/');
        CY_REQUIRE(assets::fs::create_directories(path.substr(0, slash).c_str()).has_value());
        CY_REQUIRE(
            assets::fs::write_atomic(path.c_str(), content.data(), content.size()).has_value());
    }

    [[nodiscard]] Expected<BuildReport, Error> build(BuildService& service) {
        provider_ = std::make_unique<DirectorySourceProvider>(temp_.path() + "/project");
        cache_root_ = temp_.path() + "/cache";
        BuildConfig config;
        config.graph = &graph_;
        config.producers = &producers_;
        config.sources = provider_.get();
        config.artefact_root = temp_.path() + "/artefacts";
        config.cache.local = cache_root_.c_str();
        config.workers = 0;
        CY_REQUIRE(service.configure(config).has_value());
        return service.build();
    }

private:
    cy::test::TempDir temp_;
    ProducerRegistry producers_;
    BuildGraph graph_;
    std::unique_ptr<DirectorySourceProvider> provider_;
    std::string cache_root_;
};

[[nodiscard]] const NodeResult* result_for(const BuildReport& report, std::string_view name) {
    for (const NodeResult& result : report.nodes) {
        if (result.name == name) {
            return &result;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string artefact_of(const BuildReport& report, const ArtefactStore& store,
                                      std::string_view node) {
    const NodeResult* result = result_for(report, node);
    CY_REQUIRE(result != nullptr);
    CY_REQUIRE(!result->outputs.empty());
    Array<u8> buffer(test_allocator());
    CY_REQUIRE(store.get(result->outputs.front().digest, buffer).has_value());
    return {reinterpret_cast<const char*>(buffer.data()), buffer.size()};
}

}  // namespace

CY_TEST_CASE("a level bakes to a cooked lightmap, and an unchanged level bakes nothing") {
    Project project("content-lightmap");
    import::Uv2Cache::process().clear();
    const u64 unwraps = import::uv2_unwrap_count();

    BuildService first;
    const Expected<BuildReport, Error> cold = project.build(first);
    CY_REQUIRE(cold.has_value());
    CY_REQUIRE(cold->succeeded());
    CY_CHECK(result_for(*cold, "import:quad")->outcome == NodeOutcome::Ran);
    CY_CHECK(result_for(*cold, "lightmap:level")->outcome == NodeOutcome::Ran);
    CY_CHECK(import::uv2_unwrap_count() == unwraps + 1);
    const std::string baked = artefact_of(*cold, first.artefacts(), "lightmap:level");

    // The artefact IS a cooked lightmap: two instances addressed, two directional planes.
    rendering::lightmap_bake::BakedLightmap decoded;
    CY_REQUIRE(rendering::lightmap_bake::decode_lightmap_asset(
                   Span<const u8>(reinterpret_cast<const u8*>(baked.data()), baked.size()), decoded)
                   .has_value());
    CY_CHECK(decoded.mode == rendering::lightmap_bake::LightmapMode::Directional);
    CY_REQUIRE(decoded.addresses.size() == 2);
    CY_CHECK(decoded.addresses[0] != rendering::lightmap_bake::kNoLightmapAddress);
    CY_CHECK(decoded.addresses[1] != decoded.addresses[0]);
    CY_CHECK(decoded.texels.planes == 2U);

    // NOTHING CHANGED: neither node runs, so neither the unwrap nor the bake does, and the bytes
    // are the same bytes.
    BuildService warm;
    const Expected<BuildReport, Error> unchanged = project.build(warm);
    CY_REQUIRE(unchanged.has_value());
    CY_CHECK(result_for(*unchanged, "import:quad")->outcome == NodeOutcome::Cached);
    CY_CHECK(result_for(*unchanged, "lightmap:level")->outcome == NodeOutcome::Cached);
    CY_CHECK(import::uv2_unwrap_count() == unwraps + 1);
    CY_CHECK_EQ(artefact_of(*unchanged, warm.artefacts(), "lightmap:level"), baked);

    // THE LIGHT CHANGED: the level re-bakes, the mesh does not re-import.
    project.write("levels/level.cylightmap", level_description(60.0F));
    BuildService relit;
    const Expected<BuildReport, Error> lit = project.build(relit);
    CY_REQUIRE(lit.has_value());
    CY_REQUIRE(lit->succeeded());
    CY_CHECK(result_for(*lit, "import:quad")->outcome == NodeOutcome::Cached);
    CY_CHECK(result_for(*lit, "lightmap:level")->outcome == NodeOutcome::Ran);
    CY_CHECK(import::uv2_unwrap_count() == unwraps + 1);
    CY_CHECK(artefact_of(*lit, relit.artefacts(), "lightmap:level") != baked);
}

CY_TEST_CASE("a lightmap description naming a mesh no upstream produced fails the node") {
    Project project("content-lightmap-missing");
    std::string broken = level_description(20.0F);
    broken +=
        "instance \"derived/quad.bundle\" \"mesh/Nothing\" \"white\" 1 "
        "1 0 0 0  0 1 0 0  0 0 1 0\n";
    project.write("levels/level.cylightmap", broken);
    BuildService service;
    const Expected<BuildReport, Error> report = project.build(service);
    CY_REQUIRE(report.has_value());
    CY_CHECK_FALSE(report->succeeded());
    CY_CHECK(result_for(*report, "lightmap:level")->outcome != NodeOutcome::Ran);
}

CY_TEST_CASE("a lightmap light line takes a mobility, and a misspelt one fails the node") {
    // No word: stationary, so the cooked lightmap has its shadow-mask channel.
    Project stationary("content-lightmap-stationary");
    BuildService first;
    const Expected<BuildReport, Error> defaulted = stationary.build(first);
    CY_REQUIRE(defaulted.has_value());
    CY_REQUIRE(defaulted->succeeded());
    const std::string with_mask = artefact_of(*defaulted, first.artefacts(), "lightmap:level");
    rendering::lightmap_bake::BakedLightmap decoded;
    CY_REQUIRE(rendering::lightmap_bake::decode_lightmap_asset(
                   Span<const u8>(reinterpret_cast<const u8*>(with_mask.data()), with_mask.size()),
                   decoded)
                   .has_value());
    CY_CHECK_EQ(decoded.shadow_lights.size(), 1U);

    // `static`: its direct term is baked, and there is no mask.
    Project fixed("content-lightmap-static");
    fixed.write("levels/level.cylightmap", level_description(20.0F, "static"));
    BuildService second;
    const Expected<BuildReport, Error> baked = fixed.build(second);
    CY_REQUIRE(baked.has_value());
    CY_REQUIRE(baked->succeeded());
    const std::string without_mask = artefact_of(*baked, second.artefacts(), "lightmap:level");
    CY_REQUIRE(
        rendering::lightmap_bake::decode_lightmap_asset(
            Span<const u8>(reinterpret_cast<const u8*>(without_mask.data()), without_mask.size()),
            decoded)
            .has_value());
    CY_CHECK(decoded.shadow_lights.empty());

    // A misspelling is refused, not baked as the default.
    Project misspelt("content-lightmap-misspelt");
    misspelt.write("levels/level.cylightmap", level_description(20.0F, "stationery"));
    BuildService third;
    const Expected<BuildReport, Error> refused = misspelt.build(third);
    CY_REQUIRE(refused.has_value());
    CY_CHECK_FALSE(refused->succeeded());
}

// --- The editor's description (`add-editor-lighting-tools`) --------------------------------------
//
// `data/editor_level/` is a level exactly as the editor writes it from an authored world — the
// Rust side's `the_bake_is_of_the_authored_world.rs` compares its own output against these files
// byte for byte. Here the engine reads and bakes them, so the two sides agree on every line.

namespace {

namespace bake = rendering::lightmap_bake;
namespace gi = rendering::gi;

/// The engine identities the editor gave the authored objects (`mirror::engine_identity`).
constexpr u64 kLamp = 8381449887243890699ULL;
constexpr u64 kSun = 13083581185696523244ULL;
constexpr u64 kCrate = 3679318588791258154ULL;
constexpr u64 kFloor = 17423931364048177225ULL;
constexpr u64 kVolume = 17785712484149155789ULL;

[[nodiscard]] std::string editor_level() {
    return std::string(CY_BUILD_TEST_DATA) + "/editor_level";
}

[[nodiscard]] std::string read_file(const std::string& path) {
    Array<u8> bytes(test_allocator());
    CY_REQUIRE(assets::fs::read_whole(path.c_str(), bytes).has_value());
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] Status read_from(void* user, std::string_view name, Array<u8>& out) {
    const std::string path = *static_cast<const std::string*>(user) + "/" + std::string(name);
    return assets::fs::read_whole(path.c_str(), out);
}

/// The editor's description, with `from` replaced by `to` once.
[[nodiscard]] std::string edited(std::string text, std::string_view from, std::string_view to) {
    const usize at = text.find(from);
    CY_REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
    return text;
}

struct EditorBake {
    LightmapBakeOutput out;
    LightmapJobReport report;
    bake::BakedLightmap lightmap;
};

[[nodiscard]] EditorBake bake_editor_level(const std::string& document) {
    std::string root = editor_level();
    const BundleSource files{&read_from, &root};
    EditorBake baked;
    const Status made =
        bake_lightmap_description(document, files, nullptr, baked.out, baked.report);
    if (!made) {
        CY_TEST_MESSAGE("the bake failed: " << made.error().message);
    }
    CY_REQUIRE(made.has_value());
    CY_REQUIRE(bake::decode_lightmap_asset(baked.out.payload.span(), baked.lightmap).has_value());
    return baked;
}

/// Two floats with the same bits: what a payload written and read back must hold.
[[nodiscard]] bool same_bits(f32 a, f32 b) {
    return std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

[[nodiscard]] bool names(const Array<u64>& ids, u64 id) {
    return std::ranges::find(ids, id) != ids.end();
}

[[nodiscard]] const bake::BakeInstance* instance_with(const LightmapLevel& level, u64 id) {
    for (const bake::BakeInstance& instance : level.instances) {
        if (instance.id == id) {
            return &instance;
        }
    }
    return nullptr;
}

}  // namespace

CY_TEST_CASE("the editor's description reads back as the world it was written from") {
    const std::string document = read_file(editor_level() + "/levels/room.cylightmap");
    std::string root = editor_level();
    const BundleSource files{&read_from, &root};
    LightmapLevel level;
    CY_REQUIRE(read_lightmap_description(document, files, level).has_value());

    CY_CHECK(level.settings.mode == bake::LightmapMode::Irradiance);
    CY_CHECK_EQ(level.settings.trace.samples, 8U);
    CY_CHECK_EQ(level.settings.trace.bounces, 1U);
    CY_CHECK_EQ(level.settings.atlas.page_size, 128U);

    // The lights, by the scene's identities, with the mobility the author set.
    CY_REQUIRE_EQ(level.lights.size(), 2U);
    CY_CHECK_EQ(level.lights[0].id, kLamp);
    CY_CHECK(level.lights[0].mobility == gi::LightMobility::Static);
    CY_CHECK_FALSE(level.lights[0].directional);
    CY_CHECK_EQ(level.lights[0].position.y, 2.0F);
    CY_CHECK_EQ(level.lights[1].id, kSun);
    CY_CHECK(level.lights[1].mobility == gi::LightMobility::Movable);
    CY_CHECK(level.lights[1].directional);

    // The instances, by identity, with their resolution and whether they own a lightmap.
    CY_REQUIRE_EQ(level.instances.size(), 2U);
    const bake::BakeInstance* floor = instance_with(level, kFloor);
    const bake::BakeInstance* crate = instance_with(level, kCrate);
    CY_REQUIRE(floor != nullptr);
    CY_REQUIRE(crate != nullptr);
    CY_CHECK_EQ(floor->resolution_scale, 2.0F);
    CY_CHECK(floor->receives_lightmap);
    CY_CHECK_EQ(crate->resolution_scale, 1.0F);
    CY_CHECK_FALSE(crate->receives_lightmap);
    CY_CHECK_EQ(crate->transform.columns[3].x, 1.0F);
    CY_CHECK_EQ(crate->transform.columns[3].z, 0.5F);
    // Both primitives were generated and unwrapped, so the floor can receive.
    CY_REQUIRE_EQ(level.meshes.size(), 2U);
    CY_CHECK_FALSE(level.meshes[floor->mesh].data.uv2.empty());

    // The volume, by identity.
    CY_REQUIRE_EQ(level.volumes.size(), 1U);
    CY_CHECK_EQ(level.volumes[0].id, kVolume);
    CY_CHECK_EQ(level.volumes[0].settings.origin.x, -1.0F);
    CY_CHECK_EQ(level.volumes[0].settings.origin.y, 0.5F);
    CY_CHECK_EQ(level.volumes[0].settings.spacing_metres, 1.0F);
    CY_CHECK_EQ(level.volumes[0].settings.count_x, 3U);
    CY_CHECK_EQ(level.volumes[0].settings.count_y, 2U);
    CY_CHECK_EQ(level.volumes[0].settings.count_z, 3U);
    CY_CHECK_EQ(level.volumes[0].settings.rays_per_probe, 32U);

    // And the two files it read are what its key is made of.
    CY_REQUIRE_EQ(level.inputs.size(), 2U);
    CY_CHECK_EQ(level.inputs[0].name, "assets/primitives/Crate.cyprim");
}

CY_TEST_CASE("a description written before identities and volumes reads as it did") {
    // No `id`, no `occluder`, no `volume`: lights numbered from one and stationary, instances from
    // zero and receiving — `cylightmap 1` exactly as #36 read it.
    const std::string document =
        "cylightmap 1\n"
        "mode directional\n"
        "light point 0 1 0 20 10 1 1 1\n"
        "light directional 0 -1 0 5 1 1 1 static\n"
        "material \"white\" 0.7 0.7 0.7\n"
        "instance \"assets/primitives/Floor.cyprim\" \"mesh\" \"white\" 1 "
        "1 0 0 0  0 1 0 0  0 0 1 0\n"
        "instance \"assets/primitives/Floor.cyprim\" \"mesh\" \"white\" 1 "
        "1 0 0 5  0 1 0 0  0 0 1 0\n";
    std::string root = editor_level();
    const BundleSource files{&read_from, &root};
    LightmapLevel level;
    CY_REQUIRE(read_lightmap_description(document, files, level).has_value());
    CY_REQUIRE_EQ(level.lights.size(), 2U);
    CY_CHECK_EQ(level.lights[0].id, 1U);
    CY_CHECK(level.lights[0].mobility == gi::LightMobility::Stationary);
    CY_CHECK_EQ(level.lights[1].id, 2U);
    CY_CHECK(level.lights[1].mobility == gi::LightMobility::Static);
    CY_REQUIRE_EQ(level.instances.size(), 2U);
    CY_CHECK_EQ(level.instances[0].id, 0U);
    CY_CHECK_EQ(level.instances[1].id, 1U);
    CY_CHECK(level.instances[1].receives_lightmap);
    CY_CHECK(level.volumes.empty());
    // One primitive read once, for both instances.
    CY_CHECK_EQ(level.meshes.size(), 1U);

    // The additions refuse what they cannot mean rather than guess.
    constexpr const char* kUnknownInstanceWord =
        "instance \"assets/primitives/Floor.cyprim\" \"mesh\" \"white\" 1 1 0 0 0  0 1 0 0  0 0 1 "
        "0 "
        "receiver\n";
    for (const char* line :
         {"light point 0 1 0 20 10 1 1 1 id 0\n", "light point 0 1 0 20 10 1 1 1 id x\n",
          "light point 0 1 0 20 10 1 1 1 static movable\n", "volume 7 0 0 0 1 0 2 2 16\n",
          "volume 7 0 0 0 0 2 2 2 16\n", "volume 7 0 0 0 1 2 2 2\n", kUnknownInstanceWord}) {
        LightmapLevel refused;
        CY_CHECK_FALSE(read_lightmap_description(document + line, files, refused).has_value());
    }
}

CY_TEST_CASE("a light's mobility, as the editor wrote it, is what the engine bakes of it") {
    const std::string document = read_file(editor_level() + "/levels/room.cylightmap");
    const EditorBake as_static = bake_editor_level(document);
    // Static: the lamp's direct term is in the texels, and it has no shadow-mask channel.
    CY_CHECK(names(as_static.lightmap.direct_lights, kLamp));
    CY_CHECK(as_static.lightmap.shadow_lights.empty());
    // The movable sun is baked into nothing.
    CY_CHECK_FALSE(names(as_static.lightmap.direct_lights, kSun));

    const EditorBake stationary =
        bake_editor_level(edited(document, " static id ", " stationary id "));
    CY_CHECK_FALSE(names(stationary.lightmap.direct_lights, kLamp));
    CY_REQUIRE_EQ(stationary.lightmap.shadow_lights.size(), 1U);
    CY_CHECK_EQ(stationary.lightmap.shadow_lights[0], kLamp);

    const EditorBake movable = bake_editor_level(edited(document, " static id ", " movable id "));
    CY_CHECK(movable.lightmap.direct_lights.empty());
    CY_CHECK(movable.lightmap.shadow_lights.empty());
    // With the lamp movable nothing lights the room, so its texels are darker than the static
    // lamp's.
    const auto total = [](const bake::BakedLightmap& lightmap) {
        f64 sum = 0.0;
        for (const Vec4& texel : lightmap.texels.texels) {
            sum += static_cast<f64>(texel.x + texel.y + texel.z);
        }
        return sum;
    };
    CY_TEST_MESSAGE("texel sums: static " << total(as_static.lightmap) << ", stationary "
                                          << total(stationary.lightmap) << ", movable "
                                          << total(movable.lightmap));
    CY_CHECK_LT(total(movable.lightmap), total(stationary.lightmap));
    CY_CHECK_LT(total(stationary.lightmap), total(as_static.lightmap));
}

CY_TEST_CASE("an object's resolution scale, as the editor wrote it, is its share of the atlas") {
    const std::string document = read_file(editor_level() + "/levels/room.cylightmap");
    const EditorBake twice = bake_editor_level(document);
    const EditorBake four = bake_editor_level(edited(document, "\"m0\" 2 ", "\"m0\" 4 "));
    // One receiver, the floor: the crate only occludes.
    CY_CHECK_EQ(twice.report.bake.objects, 1U);
    bake::AtlasPlacement at_two;
    bake::AtlasPlacement at_four;
    // The floor is the second instance: the editor writes instances in identity order.
    CY_REQUIRE(bake::decode_address(twice.lightmap.addresses[1], at_two));
    CY_REQUIRE(bake::decode_address(four.lightmap.addresses[1], at_four));
    CY_TEST_MESSAGE("the floor's rectangle: " << at_two.block_width << "x" << at_two.block_height
                                              << " blocks at scale 2, " << at_four.block_width
                                              << "x" << at_four.block_height << " at scale 4");
    CY_CHECK_GT(at_four.block_width * at_four.block_height,
                at_two.block_width * at_two.block_height);
    CY_CHECK_GT(four.report.bake.texels_covered, twice.report.bake.texels_covered);
    CY_CHECK_EQ(twice.lightmap.addresses[0], bake::kNoLightmapAddress);
}

CY_TEST_CASE("the description's volume is captured by the bake's tracer, probe for probe") {
    const std::string document = read_file(editor_level() + "/levels/room.cylightmap");
    const EditorBake baked = bake_editor_level(document);
    CY_CHECK_EQ(baked.report.volumes, 1U);
    CY_CHECK_EQ(baked.report.probes, 18U);
    bake::BakedProbes probes;
    CY_REQUIRE(bake::decode_probe_asset(baked.out.probes.span(), probes).has_value());
    CY_REQUIRE_EQ(probes.volumes.size(), 1U);
    CY_CHECK_EQ(probes.volumes[0].id, kVolume);

    // The same volume, captured directly over the same level.
    std::string root = editor_level();
    const BundleSource files{&read_from, &root};
    LightmapLevel level;
    CY_REQUIRE(read_lightmap_description(document, files, level).has_value());
    gi::IrradianceVolume direct;
    CY_REQUIRE(direct.configure(level.volumes[0].settings).has_value());
    gi::IrradianceVolume* volumes[] = {&direct};
    u64 rays = 0;
    bake::LightmapBakeReport report;
    CY_REQUIRE(bake::capture_irradiance_volumes(level.scene(), level.settings,
                                                Span<gi::IrradianceVolume* const>(volumes, 1), rays,
                                                report)
                   .has_value());
    CY_REQUIRE_EQ(probes.probes.size(), static_cast<usize>(direct.probe_count()));
    u32 lit = 0;
    for (u32 index = 0; index < direct.probe_count(); ++index) {
        const gi::VolumeProbe& expected = direct.probe(index);
        const gi::VolumeProbe& written = probes.probes[index];
        CY_CHECK(std::ranges::equal(expected.payload, written.payload, same_bits));
        CY_CHECK_EQ(expected.validity, written.validity);
        lit += expected.payload[0] > 0.0F ? 1U : 0U;
    }
    CY_CHECK_GT(lit, 0U);
}

CY_TEST_CASE("an unchanged level keys the same, and any file it reads changes its key") {
    cy::test::TempDir temp("lightmap-key");
    CY_REQUIRE(temp.valid());
    std::string root = temp.path();
    for (const char* name : {"levels/room.cylightmap", "assets/primitives/Floor.cyprim",
                             "assets/primitives/Crate.cyprim"}) {
        const std::string text = read_file(editor_level() + "/" + name);
        const std::string path = root + "/" + name;
        CY_REQUIRE(
            assets::fs::create_directories(path.substr(0, path.rfind('/')).c_str()).has_value());
        CY_REQUIRE(assets::fs::write_atomic(path.c_str(), text.data(), text.size()).has_value());
    }
    const std::string document = read_file(root + "/levels/room.cylightmap");
    const BundleSource files{&read_from, &root};
    const auto key_of = [&](const std::string& text) {
        LightmapLevel level;
        CY_REQUIRE(read_lightmap_description(text, files, level).has_value());
        const Expected<assets::DerivationKey, Error> key = lightmap_level_key(text, level);
        CY_REQUIRE(key.has_value());
        return *key;
    };
    const assets::DerivationKey first = key_of(document);
    CY_CHECK(key_of(document) == first);
    CY_CHECK(key_of(edited(document, " static id ", " movable id ")) != first);
    // A primitive the description only names by path: its bytes are in the key.
    const std::string crate = "cyprim 1\nshape box\nname Crate\norigin base\nextent 1 2 1\n";
    const std::string path = root + "/assets/primitives/Crate.cyprim";
    CY_REQUIRE(assets::fs::write_atomic(path.c_str(), crate.data(), crate.size()).has_value());
    CY_CHECK(key_of(document) != first);
}

namespace {

constexpr const char* kVolumeDescription =
    "cybuild 1\n"
    "node \"import:quad\" import \"import\" 1\n"
    "  source \"assets/quad.gltf\"\n"
    "  output \"derived/quad.bundle\"\n"
    "  option \"variant\" \"desktop\"\n"
    "  option \"opt.generate-lightmap-uvs\" \"true\"\n"
    "node \"lightmap:level\" cook \"lightmap\" 1\n"
    "  source \"levels/level.cylightmap\"\n"
    "  upstream \"import:quad\"\n"
    "  output \"derived/level.lightmap\"\n"
    "  output \"derived/level.cyprobes\"\n";

}  // namespace

CY_TEST_CASE("a lightmap node with a probe output captures the description's volumes") {
    Project project("content-lightmap-volumes", kVolumeDescription);
    project.write("levels/level.cylightmap",
                  level_description(20.0F) + "volume 9 -1 0.5 0.5 1 2 1 2 16\n");
    BuildService service;
    const Expected<BuildReport, Error> report = project.build(service);
    CY_REQUIRE(report.has_value());
    CY_REQUIRE(report->succeeded());
    const NodeResult* result = result_for(*report, "lightmap:level");
    CY_REQUIRE(result != nullptr);
    CY_REQUIRE_EQ(result->outputs.size(), 2U);
    const auto probes_output = std::ranges::find_if(result->outputs, [](const NodeOutput& output) {
        return output.name.ends_with(".cyprobes");
    });
    CY_REQUIRE(probes_output != result->outputs.end());
    Array<u8> bytes(test_allocator());
    CY_REQUIRE(service.artefacts().get(probes_output->digest, bytes).has_value());
    bake::BakedProbes probes;
    CY_REQUIRE(bake::decode_probe_asset(bytes.span(), probes).has_value());
    CY_REQUIRE_EQ(probes.volumes.size(), 1U);
    CY_CHECK_EQ(probes.volumes[0].id, 9U);
    CY_CHECK_EQ(probes.probes.size(), 4U);

    // The same description on a node with nowhere to put the probes fails it, by name.
    Project single("content-lightmap-volumes-single");
    single.write("levels/level.cylightmap",
                 level_description(20.0F) + "volume 9 -1 0.5 0.5 1 2 1 2 16\n");
    BuildService refused;
    const Expected<BuildReport, Error> failed = single.build(refused);
    CY_REQUIRE(failed.has_value());
    CY_CHECK_FALSE(failed->succeeded());
}
