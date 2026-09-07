#pragma once
// The cooked virtual geometry asset: its page format, its encoding, and its serialisation.
// M7 tasks 7.1, 7.2 and 7.5.
//
// `virtual-geometry` — "Virtual geometry asset": "bounds, the cluster hierarchy description, a page
// table, material ranges, an always-resident root region, a set of streamable pages, and a
// reference to a **fallback mesh**".
//
// ================================================================================================
// THE PAGE IS THE UNIT OF EVERYTHING, AND THE FORMAT IS SHAPED AROUND THAT
// ================================================================================================
//
// "Clusters SHALL be packed into **pages**: the unit of streaming, compression, caching, content
// addressing, and eviction", and each page must be "independently: hashable for content addressing,
// compressible, streamable, cacheable, and patchable".
//
// Independently is the load-bearing word. A page's payload therefore references nothing outside
// itself: positions are quantised against each CLUSTER's own bounds and UVs against its own UV
// range, a cluster's indices are local to its own vertex run, and the page carries no offset into
// any other page. So a page's bytes are a function of its own clusters' geometry alone, which is
// what makes the content hash mean "this is the same geometry" across two assets that happen to
// share a part.
//
// PER CLUSTER RATHER THAN PER PAGE, and the reason is task 7.5. The specification allows either —
// "quantised relative to cluster or page bounds" — but a page-wide frame makes every cluster's
// bytes depend on every other cluster in the page, so editing one corner of a mesh rewrites whole
// pages that did not change. Per cluster, the encoding of a cluster is a function of that cluster,
// and the cook is cache-friendly at the granularity task 7.5 names.
//
// ================================================================================================
// WHY THE HEADER CARRIES THE POLICY
// ================================================================================================
//
// "Cluster size SHALL be a cooker policy ... not a constant baked into the format — so it can be
// tuned per platform and evolved without a format change." A reader that assumed 128 triangles
// would be a reader that has to change when the policy does. The policy is therefore data in the
// header and the reader consults it, which is the difference between "recookable without a format
// version change" and a format version change with extra steps.

#include <cy/core/assets/derivation.h>
#include <cy/core/assets/hash.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/virtual_geometry/build.h>
#include <cy/rendering/virtual_geometry/cluster.h>

#include <string_view>

namespace cy::rendering::vg {

/// 'CYVG'. The first four bytes of every cooked asset, written as bytes rather than as a word so
/// that the file begins with the same four characters on either byte order.
inline constexpr u32 kAssetMagic = 0x47565943U;  // 'C' 'Y' 'V' 'G'
inline constexpr u32 kAssetVersion = 1;

/// How a vertex is quantised inside a page. `virtual-geometry` — "Geometry compression": positions
/// "quantised relative to cluster or page bounds rather than stored as world-space 32-bit floats",
/// normals "octahedral-encoded at a declared bit depth", UVs "quantised relative to a per-cluster
/// range".
///
/// Declared per asset and recorded in the header, because "Encoding precision SHALL be a cooker
/// policy with reported error, so quality is a decision rather than a default".
struct VertexEncoding {
    u32 position_bits = 16;
    u32 normal_bits = 10;
    u32 uv_bits = 12;
    bool has_normals = true;
    bool has_uvs = true;

    /// Bits per vertex under this encoding. Rounded up to a byte per attribute rather than packed
    /// across attribute boundaries: a bit-exact pack saves under a byte per vertex and costs the
    /// shader a shift chain per attribute, and the shader runs per visible vertex per frame.
    [[nodiscard]] u32 vertex_bytes() const noexcept;
};

/// Fixed cost per cluster in a page, before its geometry. The requirement asks for it to be
/// reported "since it is paid for every cluster in every asset", so it is a named constant rather
/// than a number that falls out of a struct's padding.
inline constexpr u32 kClusterMetadataBytes = 144;

/// Bytes one cluster occupies in a page: its metadata, its vertex run, and its byte indices.
[[nodiscard]] u32 encoded_cluster_bytes(u32 vertex_count, u32 index_count,
                                        const VertexEncoding& encoding) noexcept;

/// The version of this cooker. `asset-import-pipeline`: "WHEN an importer's version increases THEN
/// all assets it handles SHALL be re-cooked, since the version is part of the derivation key."
/// Raise it whenever the bytes this module produces from one input change.
inline constexpr u32 kCookerVersion = 1;

inline constexpr const char* kCookerName = "cy.virtual-geometry";

/// The derivation key for one cooked asset, built through `cy::assets::DerivationKeyBuilder` and
/// through `ToolchainFingerprint::contribute` — the single key M7 task 1.1 made reachable from
/// every producer in the tree.
///
/// `source` is the content hash of the mesh the cook read. Every field of `BuildOptions` that can
/// change the output contributes, in a fixed order, in ONE function — which is the trap
/// derivation.h names: a producer whose key is assembled where each input is discovered will miss
/// its own cache the first time somebody reorders the discovery.
///
/// Fails when the toolchain fingerprint is incomplete rather than keying against nothing.
[[nodiscard]] Expected<assets::DerivationKey, Error> derive_geometry_key(
    const assets::ContentHash& source, const BuildOptions& options,
    std::string_view variant) noexcept;

/// Everything a reader gets back from a cooked asset. Non-owning over `bytes`: the page payloads
/// are slices of the caller's buffer, so decoding a hundred-megabyte asset copies the header and
/// the cluster table and nothing else.
struct DecodedAsset {
    explicit DecodedAsset(Allocator& allocator) noexcept;

    DecodedAsset(const DecodedAsset&) = delete;
    DecodedAsset& operator=(const DecodedAsset&) = delete;
    DecodedAsset(DecodedAsset&&) noexcept = default;
    DecodedAsset& operator=(DecodedAsset&&) noexcept = default;

    Array<Cluster> clusters;
    Array<PageDescription> pages;
    Array<u32> cluster_children;
    Aabb bounds;
    ClusterPolicy policy;
    VertexEncoding encoding;
    DeformationClass deformation = DeformationClass::Static;
    SurfaceClass surface = SurfaceClass::Solid;
    TangentPolicy tangents = TangentPolicy::Derived;
    u32 source_triangles = 0;
    u32 levels = 0;
    u32 resident_bytes = 0;
    /// The whole payload region, so a page's bytes are `payload.subspan(page.byte_offset,
    /// page.byte_size)`.
    Span<const u8> payload;
};

/// One decoded cluster's geometry, in the caller's own arrays. Used by the fallback path, by the
/// watertightness check and by the tests; the GPU path uploads page bytes without decoding them.
struct DecodedCluster {
    explicit DecodedCluster(Allocator& allocator) noexcept;

    DecodedCluster(const DecodedCluster&) = delete;
    DecodedCluster& operator=(const DecodedCluster&) = delete;
    DecodedCluster(DecodedCluster&&) noexcept = default;
    DecodedCluster& operator=(DecodedCluster&&) noexcept = default;

    Array<Vec3> positions;
    Array<Vec3> normals;
    Array<Vec2> uvs;
    Array<u32> indices;  // cluster-local
};

/// Serialise a build. Deterministic: the same build produces the same bytes, and two builds of the
/// same mesh under the same options produce the same build.
///
/// Fills in each page's content hash as it goes, and writes the reported figures back into
/// `build` — cooked size, resident size, bytes per triangle, quantisation error — because they are
/// properties of the encoding and are not known until it has run.
[[nodiscard]] Status encode_asset(GeometryBuild& build, const VertexEncoding& encoding,
                                  Array<u8>& out) noexcept;

[[nodiscard]] Expected<DecodedAsset, Error> decode_asset(
    Span<const u8> bytes, Allocator& allocator = current_allocator()) noexcept;

/// Decode one cluster's geometry out of a page's payload.
/// One cluster's content hash: its quantisation frame and its encoded geometry, and nothing else.
///
/// TASK 7.5's UNIT IS THE CLUSTER, and this is the function that makes that measurable. The page is
/// the unit of streaming and of content addressing — the specification says so — but a page is a
/// RUN of clusters, so a recook that produces one more triangle in one group shifts every page
/// boundary after it while leaving the clusters themselves untouched. A cache that compared pages
/// would report a total miss on an edit that changed a hundredth of the geometry.
///
/// So the cook is cache-friendly at cluster granularity and the diagnostic measures it there: this
/// hash covers the cluster's own bounds, its own UV range and its own encoded vertices and indices,
/// which is everything its bytes depend on and nothing else. `test_asset.cpp` edits one corner of a
/// mesh, recooks, and asserts the fraction of clusters whose hash survives.
[[nodiscard]] Expected<assets::ContentHash, Error> cluster_content_hash(const DecodedAsset& asset,
                                                                        u32 cluster_index) noexcept;

[[nodiscard]] Expected<DecodedCluster, Error> decode_cluster(
    const DecodedAsset& asset, u32 cluster_index,
    Allocator& allocator = current_allocator()) noexcept;

}  // namespace cy::rendering::vg
