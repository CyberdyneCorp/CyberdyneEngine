// SPDX-License-Identifier: MIT
// The navmesh producer: a saved navigation bake as a node in the derivation graph. Issue #28,
// task 2.4. `integration.build_content`.

#include <cy/build/content_producers.h>
#include <cy/build/description.h>
#include <cy/build/key.h>
#include <cy/build/service.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/navigation/bake.h>
#include <cy/navigation/bake_codec.h>
#include <cy/navigation/tile_identity.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

using namespace cy;
using namespace cy::build;
namespace nav = cy::navigation;

namespace {

[[nodiscard]] Allocator& test_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

/// A saved bake of `width` metres of flat ground, one 8 m tile per 8 m, and its identity.
struct Sidecar {
    std::string bytes;
    u64 identity = 0;
};

[[nodiscard]] Sidecar bake_ground(u32 width) {
    nav::NavBakeSettings settings;
    settings.cell_size = 0.5F;
    settings.agent_radius = 0.25F;
    settings.tile_size = 8.0F;
    Array<Vec3> vertices(test_allocator());
    Array<u32> indices(test_allocator());
    const f32 w = static_cast<f32>(width);
    for (const Vec3 corner : {Vec3{0.0F, 0.0F, 0.0F}, Vec3{w, 0.0F, 8.0F}, Vec3{w, 0.0F, 0.0F},
                              Vec3{0.0F, 0.0F, 8.0F}}) {
        CY_REQUIRE(vertices.push_back(corner).has_value());
    }
    for (const u32 index : {0U, 1U, 2U, 0U, 3U, 1U}) {
        CY_REQUIRE(indices.push_back(index).has_value());
    }
    nav::NavBakeSource source;
    source.geometry.vertices = vertices.span();
    source.geometry.indices = indices.span();
    nav::NavMesh mesh(test_allocator(), Name::intern("test.navmesh"), settings.tile_size);
    const Aabb region = Aabb::from_min_max(Vec3{0.0F, -1.0F, 0.0F}, Vec3{w, 1.0F, 8.0F});
    CY_REQUIRE(
        nav::bake_tiles(test_allocator(), settings, source, region, mesh, nullptr).has_value());
    const u64 fingerprint = nav::source_fingerprint(settings, source, 1);
    Sidecar out;
    Array<u8> encoded(test_allocator());
    CY_REQUIRE(nav::encode_nav_bake(settings, fingerprint, mesh, encoded).has_value());
    const auto identity = nav::mesh_bake_identity(fingerprint, mesh);
    CY_REQUIRE(identity.has_value());
    out.identity = identity.has_value() ? *identity : 0;
    out.bytes.assign(reinterpret_cast<const char*>(encoded.data()), encoded.size());
    return out;
}

[[nodiscard]] std::string description(u64 identity) {
    std::string text = R"(cybuild 1
node "navmesh:level" cook "navmesh"
  source "navigation/level.cynavmesh"
  output "derived/level.navmesh"
)";
    text += R"(  option "bake-identity" ")" + std::to_string(identity) + "\"\n";
    return text;
}

class Project {
public:
    Project(const char* label, const Sidecar& sidecar, u64 identity) : temp_(label) {
        CY_REQUIRE(temp_.valid());
        CY_REQUIRE(producers_.add_builtins().has_value());
        CY_REQUIRE(add_content_producers(producers_, nullptr).has_value());
        CY_REQUIRE(read_description(description(identity), graph_, &producers_).has_value());
        write("navigation/level.cynavmesh", sidecar.bytes);
    }

    void write(const std::string& name, std::string_view content) const {
        const std::string path = temp_.path() + "/project/" + name;
        const usize slash = path.rfind('/');
        CY_REQUIRE(assets::fs::create_directories(path.substr(0, slash).c_str()).has_value());
        CY_REQUIRE(
            assets::fs::write_atomic(path.c_str(), content.data(), content.size()).has_value());
    }

    /// Rereads the graph with another identity option, as a cook of an edited world would.
    void describe(u64 identity) {
        graph_ = BuildGraph{};
        CY_REQUIRE(read_description(description(identity), graph_, &producers_).has_value());
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

/// The navmesh node's result; an empty one, and a failed check, when the report lacks it.
[[nodiscard]] const NodeResult& level_of(const BuildReport& report) {
    static const NodeResult missing;
    for (const NodeResult& result : report.nodes) {
        if (result.name == "navmesh:level") {
            return result;
        }
    }
    CY_CHECK(false);
    return missing;
}

[[nodiscard]] bool diagnosed(const NodeResult& result, std::string_view code) {
    return std::ranges::any_of(result.diagnostics, [code](const Diagnostic& diagnostic) {
        return diagnostic.code == code;
    });
}

}  // namespace

CY_TEST_CASE("navmesh producer: a valid sidecar cooks, and an unchanged one is cached") {
    const Sidecar sidecar = bake_ground(16);
    Project project("content-navmesh", sidecar, sidecar.identity);
    BuildService cold;
    const Expected<BuildReport, Error> first = project.build(cold);
    CY_REQUIRE(first.has_value());
    if (!first.has_value()) {
        return;
    }
    CY_REQUIRE(first->succeeded());
    const NodeResult& ran = level_of(*first);
    CY_CHECK(ran.outcome == NodeOutcome::Ran);
    CY_REQUIRE_EQ(ran.outputs.size(), usize{1});
    if (ran.outputs.size() != 1) {
        return;
    }

    // The artefact is a verified navigation mesh that installs: two tiles, the saved identity.
    Array<u8> cooked(test_allocator());
    CY_REQUIRE(cold.artefacts().get(ran.outputs.front().digest, cooked).has_value());
    const auto asset = nav::decode_nav_bake(test_allocator(), cooked.span());
    CY_REQUIRE(asset.has_value());
    if (!asset.has_value()) {
        return;
    }
    CY_CHECK_EQ(asset->bake_identity, sidecar.identity);
    CY_CHECK_EQ(asset->tiles.size(), usize{2});

    BuildService warm;
    const Expected<BuildReport, Error> again = project.build(warm);
    CY_REQUIRE(again.has_value());
    if (!again.has_value()) {
        return;
    }
    CY_CHECK(level_of(*again).outcome == NodeOutcome::Cached);
    CY_CHECK(level_of(*again).key == ran.key);
}

CY_TEST_CASE("navmesh producer: a rebake changes the node key") {
    const Sidecar first_bake = bake_ground(16);
    Project project("content-navmesh-rebake", first_bake, first_bake.identity);
    BuildService first;
    const Expected<BuildReport, Error> before = project.build(first);
    CY_REQUIRE(before.has_value());
    if (!before.has_value()) {
        return;
    }
    CY_REQUIRE(before->succeeded());

    const Sidecar second_bake = bake_ground(24);
    CY_REQUIRE_NE(second_bake.identity, first_bake.identity);
    project.write("navigation/level.cynavmesh", second_bake.bytes);
    project.describe(second_bake.identity);
    BuildService second;
    const Expected<BuildReport, Error> after = project.build(second);
    CY_REQUIRE(after.has_value());
    if (!after.has_value()) {
        return;
    }
    CY_REQUIRE(after->succeeded());
    CY_CHECK(level_of(*after).outcome == NodeOutcome::Ran);
    CY_CHECK(level_of(*after).key != level_of(*before).key);
}

CY_TEST_CASE("navmesh producer: a corrupt tile or an identity mismatch fails the node") {
    const Sidecar sidecar = bake_ground(16);
    Sidecar corrupt = sidecar;
    // Past the 77-byte header and the first tile's 40-byte head, inside its vertex data.
    CY_REQUIRE(corrupt.bytes.size() > 160);
    corrupt.bytes[140] = static_cast<char>(corrupt.bytes[140] ^ 0x40);
    Project flipped("content-navmesh-corrupt", corrupt, sidecar.identity);
    BuildService first;
    const Expected<BuildReport, Error> refused = flipped.build(first);
    CY_REQUIRE(refused.has_value());
    if (!refused.has_value()) {
        return;
    }
    CY_CHECK_FALSE(refused->succeeded());
    CY_CHECK(level_of(*refused).outcome != NodeOutcome::Ran);
    CY_CHECK(diagnosed(level_of(*refused), "navmesh-sidecar"));

    Project mismatched("content-navmesh-identity", sidecar, sidecar.identity + 1);
    BuildService second;
    const Expected<BuildReport, Error> other = mismatched.build(second);
    CY_REQUIRE(other.has_value());
    if (!other.has_value()) {
        return;
    }
    CY_CHECK_FALSE(other->succeeded());
    CY_CHECK(diagnosed(level_of(*other), "navmesh-identity"));
}

CY_TEST_CASE("navmesh producer: a producer version bump changes the node key") {
    NodeDesc node;
    node.kind = NodeKind::Cook;
    node.name = "navmesh:level";
    node.producer = "navmesh";
    node.producer_version = kNavmeshProducerVersion;
    node.sources = {"navigation/level.cynavmesh"};
    node.outputs = {"derived/level.navmesh"};
    ToolchainFingerprint toolchain;
    toolchain.compiler = "Clang 18.1.3";
    toolchain.flags = "-O2";
    toolchain.standard_library = "libc++ 180000";
    toolchain.target = "Darwin arm64";
    toolchain.libraries = "blake3=1.8.7;zstd=1.5.7";
    const Sidecar sidecar = bake_ground(16);
    KeyInputs inputs;
    inputs.node = &node;
    inputs.toolchain = &toolchain;
    inputs.sources = {
        {node.sources.front(), assets::content_hash(sidecar.bytes.data(), sidecar.bytes.size())}};
    const auto current = derivation_key(inputs);
    node.producer_version = kNavmeshProducerVersion + 1;
    const auto bumped = derivation_key(inputs);
    CY_REQUIRE(current.has_value());
    CY_REQUIRE(bumped.has_value());
    if (current.has_value() && bumped.has_value()) {
        CY_CHECK(*current != *bumped);
    }
}
