// The lightmap bake as a node in the derivation graph. `integration.build_content`.
//
// `rendering-global-illumination` — "Lightmap baking": a cooked, content-keyed lightmap, so that
// "unchanged geometry and lighting SHALL reuse previous results"; and "UV2 and chart packing",
// "Unwrap is cached": a rebuild that changes nothing runs neither the bake nor the unwrap.

#include <cy/build/content_producers.h>
#include <cy/build/description.h>
#include <cy/build/service.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/mesh.h>
#include <cy/rendering/lightmap_bake/asset.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

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

/// A wall of four quads' size standing on a floor quad, lit by one point light.
[[nodiscard]] std::string level_description(float intensity) {
    std::string text =
        "cylightmap 1\n"
        "mode directional\n"
        "bounces 1\n"
        "samples 8\n"
        "density 4\n"
        "page 128\n"
        "sky 0.2 0.25 0.3\n";
    text += "light point 0 1 1.5 " + std::to_string(intensity) + " 20 1 1 1\n";
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
    explicit Project(const char* label) : temp_(label) {
        CY_REQUIRE(temp_.valid());
        CY_REQUIRE(producers_.add_builtins().has_value());
        CY_REQUIRE(add_content_producers(producers_, nullptr).has_value());
        CY_REQUIRE(read_description(kDescription, graph_, &producers_).has_value());
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
    CY_REQUIRE(result->outputs.size() == 1);
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
