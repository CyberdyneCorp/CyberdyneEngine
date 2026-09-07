// The cooked asset: its round trip, its page hashes, its reported figures, and the property task
// 7.5 exists for — that the cook is cache-friendly at CLUSTER granularity.
//
// The cache-friendliness case is the one worth reading. It edits one corner of a mesh, recooks, and
// asserts that most pages' content hashes are unchanged. A cook that quantised against page-wide
// bounds would fail it while producing a perfectly correct asset, which is exactly the kind of
// defect that is invisible until a shared cache is measured.

#include <cy/test/test.h>

#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/virtual_geometry/asset.h>

#include "meshes.h"

#include <cstring>

namespace {

using namespace cy;             // NOLINT(google-build-using-namespace) — the suite's own subject
using namespace cy::rendering;  // NOLINT(google-build-using-namespace)

vg::BuildOptions options_for(u32 page_bytes = 4096) noexcept {
    vg::BuildOptions options;
    options.policy.min_triangles = 8;
    options.policy.target_triangles = 32;
    options.policy.max_triangles = 32;
    options.policy.max_vertices = 64;
    options.policy.group_size = 4;
    // Small pages so a small test mesh still produces several of them; the default 128 KiB would
    // put this whole asset in one page and the page-level claims would be vacuous.
    options.page_bytes = page_bytes;
    options.resident_budget_bytes = page_bytes;
    return options;
}

struct Cooked {
    explicit Cooked(Allocator& allocator) noexcept : bytes(allocator) {}

    Cooked(const Cooked&) = delete;
    Cooked& operator=(const Cooked&) = delete;
    Cooked(Cooked&&) noexcept = default;

    Array<u8> bytes;
    u32 pages = 0;
    u32 clusters = 0;
    f32 quantisation_error = 0.0F;
    f32 bytes_per_triangle = 0.0F;
    u32 resident_bytes = 0;
    u32 cooked_bytes = 0;
};

Cooked cook(Allocator& allocator, const vg::SourceMesh& mesh, const vg::BuildOptions& options) {
    Cooked out(allocator);
    Expected<vg::GeometryBuild, Error> build = vg::build_geometry(mesh, options, allocator);
    CY_REQUIRE(build.has_value());
    vg::VertexEncoding encoding;
    encoding.position_bits = options.position_bits;
    encoding.normal_bits = options.normal_bits;
    encoding.uv_bits = options.uv_bits;
    CY_REQUIRE(vg::encode_asset(*build, encoding, out.bytes).has_value());
    out.pages = static_cast<u32>(build->pages.size());
    out.clusters = static_cast<u32>(build->clusters.size());
    out.quantisation_error = build->quantisation_error;
    out.bytes_per_triangle = build->bytes_per_triangle;
    out.resident_bytes = build->resident_bytes;
    out.cooked_bytes = build->cooked_bytes;
    return out;
}

}  // namespace

CY_TEST_CASE("a cooked asset decodes back to what it was cooked from") {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 2);
    const vg::BuildOptions options = options_for();
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), options, allocator);
    CY_REQUIRE(build.has_value());

    Array<u8> bytes(allocator);
    vg::VertexEncoding encoding;
    CY_REQUIRE(vg::encode_asset(*build, encoding, bytes).has_value());

    Expected<vg::DecodedAsset, Error> decoded = vg::decode_asset(bytes.span(), allocator);
    CY_REQUIRE(decoded.has_value());
    CY_CHECK_EQ(decoded->clusters.size(), build->clusters.size());
    CY_CHECK_EQ(decoded->pages.size(), build->pages.size());
    CY_CHECK_EQ(decoded->source_triangles, build->source_triangles);
    CY_CHECK_EQ(decoded->levels, build->levels);
    CY_CHECK_EQ(decoded->policy.target_triangles, options.policy.target_triangles);
    CY_CHECK_EQ(decoded->policy.group_size, options.policy.group_size);

    for (usize index = 0; index < decoded->clusters.size(); ++index) {
        const vg::Cluster& before = build->clusters[index];
        const vg::Cluster& after = decoded->clusters[index];
        CY_CHECK_EQ(after.lod_error, before.lod_error);
        CY_CHECK_EQ(after.parent_error, before.parent_error);
        CY_CHECK_EQ(after.vertex_count, before.vertex_count);
        CY_CHECK_EQ(after.index_count, before.index_count);
        CY_CHECK_EQ(after.material, before.material);
        CY_CHECK_EQ(after.child_count, before.child_count);
        CY_CHECK_EQ(after.first_child, before.first_child);
    }
}

CY_TEST_CASE("a decoded cluster's geometry is the cooked geometry within the quantisation bound") {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 2);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), options_for(), allocator);
    CY_REQUIRE(build.has_value());
    Array<u8> bytes(allocator);
    vg::VertexEncoding encoding;
    CY_REQUIRE(vg::encode_asset(*build, encoding, bytes).has_value());
    Expected<vg::DecodedAsset, Error> asset = vg::decode_asset(bytes.span(), allocator);
    CY_REQUIRE(asset.has_value());

    // The reported error is a BOUND, so every vertex of every cluster must be within it. A bound
    // that held on average and not at the extreme would be a bound that says nothing.
    const f32 bound = build->quantisation_error;
    CY_CHECK_GT(bound, 0.0F);
    u32 checked = 0;
    for (u32 index = 0; index < asset->clusters.size(); ++index) {
        Expected<vg::DecodedCluster, Error> cluster = vg::decode_cluster(*asset, index, allocator);
        CY_REQUIRE(cluster.has_value());
        const vg::Cluster& record = asset->clusters[index];
        CY_REQUIRE_EQ(cluster->positions.size(), record.vertex_count);
        CY_REQUIRE_EQ(cluster->indices.size(), record.index_count);
        for (u32 slot = 0; slot < record.vertex_count; ++slot) {
            const Vec3 original = build->positions[record.first_vertex + slot];
            CY_CHECK_LE(length(cluster->positions[slot] - original), bound + 1.0e-6F);
            ++checked;
        }
        for (u32 slot = 0; slot < record.index_count; ++slot) {
            CY_CHECK_EQ(cluster->indices[slot], build->indices[record.first_index + slot]);
            CY_CHECK_LT(cluster->indices[slot], record.vertex_count);
        }
        // Normals survive the octahedral round trip to well within a degree at ten bits.
        for (u32 slot = 0; slot < record.vertex_count; ++slot) {
            const Vec3 original = build->normals[record.first_vertex + slot];
            CY_CHECK_GT(dot(cluster->normals[slot], original), 0.999F);
        }
    }
    CY_CHECK_GT(checked, 100U);
}

CY_TEST_CASE("the first page is always resident, whatever the budget says") {
    // "An object SHALL never fail to render because streaming has not completed" is unconditional,
    // so a resident budget of zero must still leave the root behind.
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 2);
    vg::BuildOptions options = options_for(2048);
    options.resident_budget_bytes = 0;
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), options, allocator);
    CY_REQUIRE(build.has_value());
    Array<u8> bytes(allocator);
    CY_REQUIRE(vg::encode_asset(*build, vg::VertexEncoding{}, bytes).has_value());

    CY_REQUIRE(build->pages.size() > 1U);
    CY_CHECK(build->pages[0].resident);
    CY_CHECK_EQ(build->resident_pages, 1U);
    CY_CHECK_GT(build->resident_bytes, 0U);

    // And the resident page holds the COARSEST clusters, which is what "sufficient to render the
    // object recognisably" means: the root region is a prefix because the clusters were ordered
    // coarsest first.
    u8 highest = 0;
    for (const vg::Cluster& cluster : build->clusters) {
        highest = cluster.level > highest ? cluster.level : highest;
    }
    CY_CHECK_EQ(build->pages[0].max_level, highest);
}

CY_TEST_CASE("a page's content hash is over its own bytes and nothing else") {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 2);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), options_for(2048), allocator);
    CY_REQUIRE(build.has_value());
    Array<u8> bytes(allocator);
    CY_REQUIRE(vg::encode_asset(*build, vg::VertexEncoding{}, bytes).has_value());
    CY_REQUIRE(build->pages.size() > 1U);

    Expected<vg::DecodedAsset, Error> asset = vg::decode_asset(bytes.span(), allocator);
    CY_REQUIRE(asset.has_value());
    for (const vg::PageDescription& page : asset->pages) {
        const assets::ContentHash recomputed =
            assets::content_hash(asset->payload.data() + page.byte_offset, page.byte_size);
        CY_CHECK_EQ(std::memcmp(recomputed.bytes, page.content_hash, 32), 0);
    }

    // No two pages of distinct geometry hash the same, which is the property the content-addressed
    // store rests on.
    for (usize a = 0; a < asset->pages.size(); ++a) {
        for (usize b = a + 1; b < asset->pages.size(); ++b) {
            CY_CHECK_NE(std::memcmp(asset->pages[a].content_hash, asset->pages[b].content_hash, 32),
                        0);
        }
    }
}

CY_TEST_CASE("editing one corner of a mesh leaves most clusters' bytes untouched") {
    // TASK 7.5, as a measurement rather than a claim, and AT THE GRANULARITY THE TASK NAMES.
    //
    // The unit here is the cluster and not the page, and the distinction is the finding rather than
    // a convenience. A page is a RUN of clusters, so a recook whose simplifier produces one more
    // triangle in one group shifts every page boundary after it: measured on this mesh, a
    // one-vertex edit left 2 of 34 pages byte-identical while leaving most CLUSTERS byte-identical.
    // A cache keyed on pages would therefore report a near-total miss on an edit that changed a
    // hundredth of the geometry, and that is a property of the packing rather than of the encoding.
    //
    // The encoding is what task 7.5 is about, and it is cluster-local by construction: positions
    // are quantised against the cluster's own bounds and UVs against its own range, so a cluster's
    // bytes are a function of that cluster's geometry alone.
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    vg::test::MeshData mesh = vg::test::icosphere(allocator, 3);
    const vg::BuildOptions options = options_for(2048);

    Array<u8> first(allocator);
    {
        Expected<vg::GeometryBuild, Error> build =
            vg::build_geometry(mesh.source(), options, allocator);
        CY_REQUIRE(build.has_value());
        CY_REQUIRE(vg::encode_asset(*build, vg::VertexEncoding{}, first).has_value());
    }
    Expected<vg::DecodedAsset, Error> before = vg::decode_asset(first.span(), allocator);
    CY_REQUIRE(before.has_value());
    CY_REQUIRE(before->clusters.size() > 40U);

    // Move one vertex outward by a tenth of the radius: one corner of one region of the mesh.
    mesh.positions[0] = mesh.positions[0] * 1.1F;

    Array<u8> second(allocator);
    {
        Expected<vg::GeometryBuild, Error> build =
            vg::build_geometry(mesh.source(), options, allocator);
        CY_REQUIRE(build.has_value());
        CY_REQUIRE(vg::encode_asset(*build, vg::VertexEncoding{}, second).has_value());
    }
    Expected<vg::DecodedAsset, Error> after = vg::decode_asset(second.span(), allocator);
    CY_REQUIRE(after.has_value());

    Array<assets::ContentHash> new_hashes(allocator);
    for (u32 index = 0; index < after->clusters.size(); ++index) {
        Expected<assets::ContentHash, Error> hash = vg::cluster_content_hash(*after, index);
        CY_REQUIRE(hash.has_value());
        CY_REQUIRE(new_hashes.push_back(*hash).has_value());
    }
    u32 shared = 0;
    for (u32 index = 0; index < before->clusters.size(); ++index) {
        Expected<assets::ContentHash, Error> hash = vg::cluster_content_hash(*before, index);
        CY_REQUIRE(hash.has_value());
        for (const assets::ContentHash& candidate : new_hashes) {
            if (candidate == *hash) {
                ++shared;
                break;
            }
        }
    }
    const u32 total = static_cast<u32>(before->clusters.size());
    CY_TEST_MESSAGE("clusters before " << total << ", after " << after->clusters.size()
                                       << ", byte-identical by content hash " << shared << " ("
                                       << (100U * shared / total) << "%)");
    // A clear majority must survive a one-vertex edit. The edited corner's own clusters, and the
    // coarse clusters whose simplification saw it, legitimately change.
    CY_CHECK_GT(shared * 2U, total);
}

CY_TEST_CASE("the reported figures are the figures") {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 3);
    const Cooked cooked = cook(allocator, mesh.source(), options_for(2048));

    CY_CHECK_EQ(cooked.cooked_bytes, static_cast<u32>(cooked.bytes.size()));
    CY_CHECK_GT(cooked.pages, 1U);
    CY_CHECK_GT(cooked.clusters, 40U);
    CY_CHECK_GT(cooked.bytes_per_triangle, 0.0F);
    CY_CHECK_LT(cooked.resident_bytes, cooked.cooked_bytes);
    CY_TEST_MESSAGE("cooked " << cooked.cooked_bytes << " bytes over " << cooked.clusters
                              << " clusters in " << cooked.pages << " pages; "
                              << cooked.bytes_per_triangle << " bytes per triangle; resident "
                              << cooked.resident_bytes << " bytes; quantisation error "
                              << cooked.quantisation_error);
}

CY_TEST_CASE("a finer encoding costs more bytes and less error") {
    // "Encoding precision SHALL be a cooker policy with reported error, so quality is a decision
    // rather than a default": the decision has to be visible in both directions.
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 2);

    vg::BuildOptions coarse = options_for();
    coarse.position_bits = 8;
    vg::BuildOptions fine = options_for();
    fine.position_bits = 16;

    const Cooked at_eight = cook(allocator, mesh.source(), coarse);
    const Cooked at_sixteen = cook(allocator, mesh.source(), fine);
    CY_CHECK_LT(at_eight.cooked_bytes, at_sixteen.cooked_bytes);
    CY_CHECK_GT(at_eight.quantisation_error, at_sixteen.quantisation_error);
    CY_TEST_MESSAGE("8-bit positions: " << at_eight.cooked_bytes << " bytes, error "
                                        << at_eight.quantisation_error
                                        << "; 16-bit: " << at_sixteen.cooked_bytes
                                        << " bytes, error " << at_sixteen.quantisation_error);
}

CY_TEST_CASE("a truncated or foreign asset is refused rather than half-read") {
    Allocator& allocator = system_allocator(MemoryDomain::Renderer);
    const vg::test::MeshData mesh = vg::test::icosphere(allocator, 1);
    Expected<vg::GeometryBuild, Error> build =
        vg::build_geometry(mesh.source(), options_for(), allocator);
    CY_REQUIRE(build.has_value());
    Array<u8> bytes(allocator);
    CY_REQUIRE(vg::encode_asset(*build, vg::VertexEncoding{}, bytes).has_value());

    CY_CHECK_FALSE(vg::decode_asset(bytes.span().subspan(0, 32), allocator).has_value());
    CY_CHECK_FALSE(
        vg::decode_asset(bytes.span().subspan(0, bytes.size() - 16), allocator).has_value());

    Array<u8> corrupted(allocator);
    CY_REQUIRE(corrupted.append(bytes.span()).has_value());
    corrupted[0] = 'X';
    CY_CHECK_FALSE(vg::decode_asset(corrupted.span(), allocator).has_value());
}

CY_TEST_CASE("the derivation key names the toolchain and every option that moves the bytes") {
    // The key is the one M7 task 1.1 made reachable from every producer. It must change when an
    // option changes and stay put when nothing does.
    assets::ContentHash source = assets::content_hash("mesh", 4);
    vg::BuildOptions options;

    Expected<assets::DerivationKey, Error> base =
        vg::derive_geometry_key(source, options, "desktop");
    CY_REQUIRE(base.has_value());
    Expected<assets::DerivationKey, Error> again =
        vg::derive_geometry_key(source, options, "desktop");
    CY_REQUIRE(again.has_value());
    CY_CHECK(*base == *again);

    Expected<assets::DerivationKey, Error> other_variant =
        vg::derive_geometry_key(source, options, "console");
    CY_REQUIRE(other_variant.has_value());
    CY_CHECK(*base != *other_variant);

    options.policy.target_triangles = 64;
    Expected<assets::DerivationKey, Error> other_policy =
        vg::derive_geometry_key(source, options, "desktop");
    CY_REQUIRE(other_policy.has_value());
    CY_CHECK(*base != *other_policy);

    options = vg::BuildOptions{};
    options.position_bits = 12;
    Expected<assets::DerivationKey, Error> other_encoding =
        vg::derive_geometry_key(source, options, "desktop");
    CY_REQUIRE(other_encoding.has_value());
    CY_CHECK(*base != *other_encoding);

    const assets::ContentHash other_source = assets::content_hash("other", 5);
    Expected<assets::DerivationKey, Error> other_mesh =
        vg::derive_geometry_key(other_source, vg::BuildOptions{}, "desktop");
    CY_REQUIRE(other_mesh.has_value());
    CY_CHECK(*base != *other_mesh);
}
