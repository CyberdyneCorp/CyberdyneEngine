#include <cy/rendering/virtual_geometry/asset.h>

#include <cy/core/assets/toolchain.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::rendering::vg {

namespace {

// ================================================================================================
// THE BYTE LAYOUT
// ================================================================================================
//
// Fixed-size little-endian records throughout, written field by field rather than by copying a
// struct: a struct copy would put the compiler's padding and the host's byte order in a file that
// two different builds have to agree about, which is the same defect class M6's toolchain
// fingerprint exists to close one level up.
//
//   header            kHeaderBytes
//   cluster table     cluster_count * kClusterMetadataBytes
//   page directory    page_count * kPageRecordBytes
//   group table       group_count * kGroupRecordBytes
//   group members     u32 each
//   group children    u32 each
//   cluster children  u32 each
//   payload           the pages, back to back
//
// A page's payload is the geometry of its clusters and nothing else. Quantisation frames are per
// CLUSTER — the cluster's own bounds for positions, its own UV range for UVs — and live in the
// cluster table, so a cluster's encoded bytes are a function of that cluster's geometry alone. That
// is what makes the cook cache-friendly at cluster granularity: editing one corner of a mesh
// changes the pages that hold it and leaves every other page's content hash untouched.

constexpr u32 kHeaderBytes = 160;
constexpr u32 kPageRecordBytes = 64;
constexpr u32 kGroupRecordBytes = 48;

class Writer {
public:
    explicit Writer(Array<u8>& out) noexcept : out_(out) {}

    [[nodiscard]] Status u8v(u8 value) noexcept { return out_.push_back(value); }

    [[nodiscard]] Status u16v(u16 value) noexcept {
        for (u32 shift = 0; shift < 16; shift += 8) {
            if (Status pushed = out_.push_back(static_cast<u8>((value >> shift) & 0xFFU));
                !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    [[nodiscard]] Status u32v(u32 value) noexcept {
        for (u32 shift = 0; shift < 32; shift += 8) {
            if (Status pushed = out_.push_back(static_cast<u8>((value >> shift) & 0xFFU));
                !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    [[nodiscard]] Status f32v(f32 value) noexcept {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        return u32v(bits);
    }

    [[nodiscard]] Status vec3(Vec3 value) noexcept {
        if (Status written = f32v(value.x); !written) {
            return written;
        }
        if (Status written = f32v(value.y); !written) {
            return written;
        }
        return f32v(value.z);
    }

    [[nodiscard]] Status vec2(Vec2 value) noexcept {
        if (Status written = f32v(value.x); !written) {
            return written;
        }
        return f32v(value.y);
    }

    [[nodiscard]] Status pad(u32 count) noexcept {
        for (u32 index = 0; index < count; ++index) {
            if (Status pushed = out_.push_back(0U); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    [[nodiscard]] usize size() const noexcept { return out_.size(); }

private:
    Array<u8>& out_;
};

class Reader {
public:
    explicit Reader(Span<const u8> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] bool has(usize wanted) const noexcept {
        return cursor_ + wanted <= bytes_.size();
    }

    [[nodiscard]] u8 u8v() noexcept { return has(1) ? bytes_[cursor_++] : 0U; }

    [[nodiscard]] u16 u16v() noexcept {
        const u16 lo = u8v();
        const u16 hi = u8v();
        return static_cast<u16>(lo | static_cast<u16>(hi << 8U));
    }

    [[nodiscard]] u32 u32v() noexcept {
        u32 value = 0;
        for (u32 shift = 0; shift < 32; shift += 8) {
            value |= static_cast<u32>(u8v()) << shift;
        }
        return value;
    }

    [[nodiscard]] f32 f32v() noexcept {
        const u32 bits = u32v();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    [[nodiscard]] Vec3 vec3() noexcept {
        const f32 x = f32v();
        const f32 y = f32v();
        const f32 z = f32v();
        return Vec3{x, y, z};
    }

    [[nodiscard]] Vec2 vec2() noexcept {
        const f32 x = f32v();
        const f32 y = f32v();
        return Vec2{x, y};
    }

    void skip(usize count) noexcept { cursor_ += count; }
    void seek(usize position) noexcept { cursor_ = position; }
    [[nodiscard]] usize position() const noexcept { return cursor_; }
    [[nodiscard]] bool overrun() const noexcept { return cursor_ > bytes_.size(); }

private:
    Span<const u8> bytes_;
    usize cursor_ = 0;
};

/// Bytes one attribute component occupies. Rounded up to a byte rather than bit-packed across
/// component boundaries: a bit-exact pack saves under a byte per vertex and costs the shader a
/// shift chain per attribute, and the shader runs per visible vertex per frame.
[[nodiscard]] u32 component_bytes(u32 bits) noexcept {
    if (bits <= 8) {
        return 1;
    }
    if (bits <= 16) {
        return 2;
    }
    return 4;
}

[[nodiscard]] u32 quantise(f32 normalised, u32 bits) noexcept {
    const f32 maximum = static_cast<f32>((1ULL << bits) - 1ULL);
    const f32 scaled = std::clamp(normalised * maximum, 0.0F, maximum);
    // `lround` rather than `+ 0.5` and a cast: the latter rounds a negative wrongly and rounds
    // 0.49999997 up, and the clamp above is what keeps the value in range rather than the cast.
    return static_cast<u32>(std::lround(scaled));
}

[[nodiscard]] f32 dequantise(u32 value, u32 bits) noexcept {
    const f32 maximum = static_cast<f32>((1ULL << bits) - 1ULL);
    return static_cast<f32>(value) / maximum;
}

/// Octahedral encoding: a unit vector becomes two numbers in [-1, 1]. `virtual-geometry` —
/// "**Normals** octahedral-encoded at a declared bit depth".
[[nodiscard]] Vec2 octahedral_encode(Vec3 normal) noexcept {
    const f32 sum = std::fabs(normal.x) + std::fabs(normal.y) + std::fabs(normal.z);
    const f32 inverse = sum > 1.0e-12F ? 1.0F / sum : 0.0F;
    f32 x = normal.x * inverse;
    f32 y = normal.y * inverse;
    if (normal.z < 0.0F) {
        const f32 folded_x = (1.0F - std::fabs(y)) * (x >= 0.0F ? 1.0F : -1.0F);
        const f32 folded_y = (1.0F - std::fabs(x)) * (y >= 0.0F ? 1.0F : -1.0F);
        x = folded_x;
        y = folded_y;
    }
    return Vec2{x, y};
}

[[nodiscard]] Vec3 octahedral_decode(Vec2 encoded) noexcept {
    Vec3 normal{encoded.x, encoded.y, 1.0F - std::fabs(encoded.x) - std::fabs(encoded.y)};
    if (normal.z < 0.0F) {
        const f32 x = (1.0F - std::fabs(normal.y)) * (normal.x >= 0.0F ? 1.0F : -1.0F);
        const f32 y = (1.0F - std::fabs(normal.x)) * (normal.y >= 0.0F ? 1.0F : -1.0F);
        normal.x = x;
        normal.y = y;
    }
    const f32 magnitude = length(normal);
    return magnitude > 1.0e-12F ? normal * (1.0F / magnitude) : Vec3{0.0F, 0.0F, 1.0F};
}

[[nodiscard]] Status write_component(Writer& writer, u32 value, u32 bits) noexcept {
    switch (component_bytes(bits)) {
        case 1:
            return writer.u8v(static_cast<u8>(value));
        case 2:
            return writer.u16v(static_cast<u16>(value));
        default:
            return writer.u32v(value);
    }
}

[[nodiscard]] u32 read_component(Reader& reader, u32 bits) noexcept {
    switch (component_bytes(bits)) {
        case 1:
            return reader.u8v();
        case 2:
            return reader.u16v();
        default:
            return reader.u32v();
    }
}

[[nodiscard]] f32 axis_span(f32 low, f32 high) noexcept {
    const f32 span = high - low;
    return span > 1.0e-12F ? span : 1.0F;
}

}  // namespace

u32 VertexEncoding::vertex_bytes() const noexcept {
    u32 total = 3U * component_bytes(position_bits);
    if (has_normals) {
        total += 2U * component_bytes(normal_bits);
    }
    if (has_uvs) {
        total += 2U * component_bytes(uv_bits);
    }
    return total;
}

u32 encoded_cluster_bytes(u32 vertex_count, u32 index_count,
                          const VertexEncoding& encoding) noexcept {
    // THE GEOMETRY BLOCK IS PADDED TO FOUR BYTES, and it is the GPU that asks for it. A shader
    // reads the page payload as a byte-addressed buffer, whose loads are word-aligned: a 16-bit
    // component can be recovered from the word containing it only when its offset is EVEN, and a
    // cluster's offset is the running sum of the earlier clusters' blocks. An odd index count would
    // make every later cluster in the page odd, and every 16-bit read in it would straddle a word.
    const u32 geometry = (vertex_count * encoding.vertex_bytes()) + index_count;
    return kClusterMetadataBytes + ((geometry + 3U) & ~3U);
}

Expected<assets::DerivationKey, Error> derive_geometry_key(const assets::ContentHash& source,
                                                           const BuildOptions& options,
                                                           std::string_view variant) noexcept {
    const assets::ToolchainFingerprint& toolchain = assets::current_toolchain();
    if (!assets::toolchain_is_complete(toolchain)) {
        // A fingerprint that quietly lost its libraries is exactly the state M6's spike found, and
        // keying against nothing would serve one compiler's pages to another's.
        return fail(ErrorCode::Unavailable,
                    "derive_geometry_key: the toolchain fingerprint is incomplete; a derivation "
                    "key that did not name the toolchain would let one build's geometry pages be "
                    "served to another's");
    }

    // ONE FUNCTION, ONE ORDER. `DerivationKeyBuilder` hashes contributions in sequence, so a
    // producer whose key is assembled where each input is discovered will miss its own cache the
    // first time somebody reorders the discovery. Everything that can change the output bytes is
    // here, and nothing that cannot.
    assets::DerivationKeyBuilder builder;
    (void)builder.producer(assets::DerivedKind::Page, kCookerName, kCookerVersion);
    toolchain.contribute(builder);
    (void)builder.source("mesh", source);
    (void)builder.text("variant", variant);
    (void)builder.number("policy.min_triangles", options.policy.min_triangles);
    (void)builder.number("policy.target_triangles", options.policy.target_triangles);
    (void)builder.number("policy.max_triangles", options.policy.max_triangles);
    (void)builder.number("policy.max_vertices", options.policy.max_vertices);
    (void)builder.number("policy.group_size", options.policy.group_size);
    (void)builder.number("policy.root_cluster_limit", options.policy.root_cluster_limit);
    (void)builder.number("policy.max_levels", options.policy.max_levels);
    (void)builder.bytes("policy.simplify_ratio", &options.policy.simplify_ratio, sizeof(f32));
    (void)builder.bytes("weld_epsilon", &options.weld_epsilon, sizeof(f32));
    (void)builder.number("page_bytes", options.page_bytes);
    (void)builder.number("resident_budget_bytes", options.resident_budget_bytes);
    (void)builder.number("position_bits", options.position_bits);
    (void)builder.number("normal_bits", options.normal_bits);
    (void)builder.number("uv_bits", options.uv_bits);
    (void)builder.number("deformation", static_cast<u64>(options.deformation));
    (void)builder.number("surface", static_cast<u64>(options.surface));
    (void)builder.number("tangents", static_cast<u64>(options.tangents));
    return builder.finish();
}

DecodedAsset::DecodedAsset(Allocator& allocator) noexcept
    : clusters(allocator), pages(allocator), cluster_children(allocator) {}

DecodedCluster::DecodedCluster(Allocator& allocator) noexcept
    : positions(allocator), normals(allocator), uvs(allocator), indices(allocator) {}

namespace {

/// Pack the clusters into pages. Clusters are already in coarsest-first order, so a page is a run
/// and the always-resident root region is a PREFIX rather than a gather.
[[nodiscard]] Status pack_pages(GeometryBuild& build, const VertexEncoding& encoding) noexcept {
    build.pages.clear();
    PageDescription page;
    page.first_cluster = 0;
    u32 bytes = 0;
    for (u32 index = 0; index < build.clusters.size(); ++index) {
        const Cluster& cluster = build.clusters[index];
        const u32 cost = encoded_cluster_bytes(cluster.vertex_count, cluster.index_count, encoding);
        if (page.cluster_count > 0 && bytes + cost > build.page_bytes) {
            page.byte_size = bytes;
            if (Status pushed = build.pages.push_back(page); !pushed) {
                return pushed;
            }
            page = PageDescription{};
            page.first_cluster = index;
            bytes = 0;
        }
        bytes += cost;
        ++page.cluster_count;
        page.max_level = cluster.level > page.max_level ? cluster.level : page.max_level;
    }
    if (page.cluster_count > 0) {
        page.byte_size = bytes;
        if (Status pushed = build.pages.push_back(page); !pushed) {
            return pushed;
        }
    }

    // `virtual-geometry` — "Always-resident root": "the hierarchy root and the coarsest clusters,
    // sufficient to render the object recognisably". The FIRST page is resident whatever the budget
    // says, because "An object SHALL never fail to render because streaming has not completed" is
    // unconditional and a budget of zero would otherwise leave nothing behind.
    u32 resident = 0;
    u32 offset = 0;
    for (u32 index = 0; index < build.pages.size(); ++index) {
        PageDescription& entry = build.pages[index];
        entry.byte_offset = offset;
        offset += entry.byte_size;
        if (index == 0 || resident + entry.byte_size <= build.resident_budget_bytes) {
            entry.resident = true;
            resident += entry.byte_size;
        }
    }
    build.resident_bytes = resident;
    build.resident_pages = 0;
    for (const PageDescription& entry : build.pages) {
        if (entry.resident) {
            ++build.resident_pages;
        }
    }
    return ok();
}

[[nodiscard]] Status write_cluster_record(Writer& writer, const Cluster& cluster,
                                          u32 page) noexcept {
    const usize start = writer.size();
    if (Status written = writer.vec3(cluster.bounds.min); !written) {
        return written;
    }
    if (Status written = writer.vec3(cluster.bounds.max); !written) {
        return written;
    }
    if (Status written = writer.vec3(cluster.cone.axis); !written) {
        return written;
    }
    if (Status written = writer.f32v(cluster.cone.cos_angle); !written) {
        return written;
    }
    if (Status written = writer.f32v(cluster.lod_error); !written) {
        return written;
    }
    if (Status written = writer.vec3(cluster.lod_sphere.center); !written) {
        return written;
    }
    if (Status written = writer.f32v(cluster.lod_sphere.radius); !written) {
        return written;
    }
    if (Status written = writer.f32v(cluster.parent_error); !written) {
        return written;
    }
    if (Status written = writer.vec3(cluster.parent_sphere.center); !written) {
        return written;
    }
    if (Status written = writer.f32v(cluster.parent_sphere.radius); !written) {
        return written;
    }
    if (Status written = writer.vec2(cluster.uv_min); !written) {
        return written;
    }
    if (Status written = writer.vec2(cluster.uv_max); !written) {
        return written;
    }
    if (Status written = writer.u32v(cluster.material); !written) {
        return written;
    }
    if (Status written = writer.u32v(page); !written) {
        return written;
    }
    if (Status written = writer.u32v(cluster.group); !written) {
        return written;
    }
    if (Status written = writer.u32v(cluster.first_child); !written) {
        return written;
    }
    if (Status written = writer.u32v(cluster.child_count); !written) {
        return written;
    }
    if (Status written = writer.u32v(cluster.first_index); !written) {
        return written;
    }
    if (Status written = writer.u32v(cluster.first_vertex); !written) {
        return written;
    }
    if (Status written = writer.u16v(static_cast<u16>(cluster.vertex_count)); !written) {
        return written;
    }
    if (Status written = writer.u16v(static_cast<u16>(cluster.index_count)); !written) {
        return written;
    }
    if (Status written = writer.u8v(cluster.level); !written) {
        return written;
    }
    const u32 used = static_cast<u32>(writer.size() - start);
    return writer.pad(kClusterMetadataBytes - used);
}

void read_cluster_record(Reader& reader, Cluster& cluster, u32& page) noexcept {
    const usize start = reader.position();
    cluster.bounds.min = reader.vec3();
    cluster.bounds.max = reader.vec3();
    cluster.cone.axis = reader.vec3();
    cluster.cone.cos_angle = reader.f32v();
    cluster.lod_error = reader.f32v();
    cluster.lod_sphere.center = reader.vec3();
    cluster.lod_sphere.radius = reader.f32v();
    cluster.parent_error = reader.f32v();
    cluster.parent_sphere.center = reader.vec3();
    cluster.parent_sphere.radius = reader.f32v();
    cluster.uv_min = reader.vec2();
    cluster.uv_max = reader.vec2();
    cluster.material = reader.u32v();
    page = reader.u32v();
    cluster.group = reader.u32v();
    cluster.first_child = reader.u32v();
    cluster.child_count = reader.u32v();
    cluster.first_index = reader.u32v();
    cluster.first_vertex = reader.u32v();
    cluster.vertex_count = reader.u16v();
    cluster.index_count = reader.u16v();
    cluster.level = reader.u8v();
    reader.seek(start + kClusterMetadataBytes);
}

/// Encode one cluster's geometry, and report the largest distance a position moved.
[[nodiscard]] Expected<f32, Error> write_cluster_geometry(Writer& writer,
                                                          const GeometryBuild& build,
                                                          const Cluster& cluster,
                                                          const VertexEncoding& encoding) noexcept {
    const Vec3 low = cluster.bounds.min;
    const Vec3 span{axis_span(cluster.bounds.min.x, cluster.bounds.max.x),
                    axis_span(cluster.bounds.min.y, cluster.bounds.max.y),
                    axis_span(cluster.bounds.min.z, cluster.bounds.max.z)};
    const Vec2 uv_low = cluster.uv_min;
    const Vec2 uv_span{axis_span(cluster.uv_min.x, cluster.uv_max.x),
                       axis_span(cluster.uv_min.y, cluster.uv_max.y)};
    f32 worst = 0.0F;

    for (u32 slot = 0; slot < cluster.vertex_count; ++slot) {
        const Vec3 position = build.positions[cluster.first_vertex + slot];
        const u32 x = quantise((position.x - low.x) / span.x, encoding.position_bits);
        const u32 y = quantise((position.y - low.y) / span.y, encoding.position_bits);
        const u32 z = quantise((position.z - low.z) / span.z, encoding.position_bits);
        if (Status written = write_component(writer, x, encoding.position_bits); !written) {
            return make_unexpected(written.error());
        }
        if (Status written = write_component(writer, y, encoding.position_bits); !written) {
            return make_unexpected(written.error());
        }
        if (Status written = write_component(writer, z, encoding.position_bits); !written) {
            return make_unexpected(written.error());
        }
        const Vec3 recovered{low.x + (dequantise(x, encoding.position_bits) * span.x),
                             low.y + (dequantise(y, encoding.position_bits) * span.y),
                             low.z + (dequantise(z, encoding.position_bits) * span.z)};
        const f32 moved = length(recovered - position);
        worst = moved > worst ? moved : worst;

        if (encoding.has_normals) {
            const Vec2 oct = octahedral_encode(build.normals[cluster.first_vertex + slot]);
            const u32 u = quantise((oct.x + 1.0F) * 0.5F, encoding.normal_bits);
            const u32 v = quantise((oct.y + 1.0F) * 0.5F, encoding.normal_bits);
            if (Status written = write_component(writer, u, encoding.normal_bits); !written) {
                return make_unexpected(written.error());
            }
            if (Status written = write_component(writer, v, encoding.normal_bits); !written) {
                return make_unexpected(written.error());
            }
        }
        if (encoding.has_uvs) {
            const Vec2 uv = build.uvs[cluster.first_vertex + slot];
            const u32 u = quantise((uv.x - uv_low.x) / uv_span.x, encoding.uv_bits);
            const u32 v = quantise((uv.y - uv_low.y) / uv_span.y, encoding.uv_bits);
            if (Status written = write_component(writer, u, encoding.uv_bits); !written) {
                return make_unexpected(written.error());
            }
            if (Status written = write_component(writer, v, encoding.uv_bits); !written) {
                return make_unexpected(written.error());
            }
        }
    }
    for (u32 slot = 0; slot < cluster.index_count; ++slot) {
        // `ClusterPolicy::validate()` caps a cluster at 256 vertices precisely so that this is a
        // byte. A wider index would be a page format change, which is why the cap is validated
        // rather than clamped.
        if (Status written = writer.u8v(static_cast<u8>(build.indices[cluster.first_index + slot]));
            !written) {
            return make_unexpected(written.error());
        }
    }
    const u32 geometry = (cluster.vertex_count * encoding.vertex_bytes()) + cluster.index_count;
    if (Status written = writer.pad((((geometry + 3U) & ~3U) - geometry)); !written) {
        return make_unexpected(written.error());
    }
    return worst;
}

}  // namespace

Status encode_asset(GeometryBuild& build, const VertexEncoding& encoding, Array<u8>& out) noexcept {
    if (build.clusters.empty()) {
        return fail(ErrorCode::InvalidArgument, "encode_asset: the build has no clusters");
    }
    VertexEncoding effective = encoding;
    effective.has_normals = encoding.has_normals && !build.normals.empty();
    effective.has_uvs = encoding.has_uvs && !build.uvs.empty();
    if (Status packed = pack_pages(build, effective); !packed) {
        return packed;
    }

    // The payload first, into its own buffer, because the header records its size and each page's
    // content hash is over bytes that do not exist until they are written.
    Array<u8> payload(out.allocator());
    Writer payload_writer(payload);
    f32 quantisation_error = 0.0F;
    Array<u32> page_of_cluster(out.allocator());
    if (Status resized = page_of_cluster.resize(build.clusters.size()); !resized) {
        return resized;
    }
    for (u32 page_index = 0; page_index < build.pages.size(); ++page_index) {
        PageDescription& page = build.pages[page_index];
        const usize start = payload.size();
        for (u32 slot = 0; slot < page.cluster_count; ++slot) {
            const u32 cluster_index = page.first_cluster + slot;
            page_of_cluster[cluster_index] = page_index;
            Expected<f32, Error> moved = write_cluster_geometry(
                payload_writer, build, build.clusters[cluster_index], effective);
            if (!moved) {
                return make_unexpected(moved.error());
            }
            quantisation_error = *moved > quantisation_error ? *moved : quantisation_error;
        }
        page.byte_offset = static_cast<u32>(start);
        page.byte_size = static_cast<u32>(payload.size() - start);
        // The content hash is over the PAGE'S OWN BYTES and nothing else. That is what lets two
        // assets that share geometry share a page in the content-addressed store, and what makes a
        // patch transfer only the pages whose content changed.
        assets::ContentHasher hasher;
        hasher.update(payload.data() + start, page.byte_size);
        const assets::ContentHash hash = hasher.finish();
        std::memcpy(page.content_hash, hash.bytes, sizeof(page.content_hash));
    }

    Writer writer(out);
    if (Status written = writer.u32v(kAssetMagic); !written) {
        return written;
    }
    if (Status written = writer.u32v(kAssetVersion); !written) {
        return written;
    }
    const u32 counts[6] = {static_cast<u32>(build.clusters.size()),
                           static_cast<u32>(build.pages.size()),
                           static_cast<u32>(build.groups.size()),
                           static_cast<u32>(build.group_members.size()),
                           static_cast<u32>(build.group_children.size()),
                           static_cast<u32>(build.cluster_children.size())};
    for (const u32 value : counts) {
        if (Status written = writer.u32v(value); !written) {
            return written;
        }
    }
    if (Status written = writer.vec3(build.bounds.min); !written) {
        return written;
    }
    if (Status written = writer.vec3(build.bounds.max); !written) {
        return written;
    }
    const u32 policy[7] = {build.policy.min_triangles, build.policy.target_triangles,
                           build.policy.max_triangles, build.policy.max_vertices,
                           build.policy.group_size,    build.policy.root_cluster_limit,
                           build.policy.max_levels};
    for (const u32 value : policy) {
        if (Status written = writer.u32v(value); !written) {
            return written;
        }
    }
    if (Status written = writer.f32v(build.policy.simplify_ratio); !written) {
        return written;
    }
    const u32 encoding_fields[3] = {effective.position_bits, effective.normal_bits,
                                    effective.uv_bits};
    for (const u32 value : encoding_fields) {
        if (Status written = writer.u32v(value); !written) {
            return written;
        }
    }
    const u8 flags[5] = {static_cast<u8>(effective.has_normals ? 1U : 0U),
                         static_cast<u8>(effective.has_uvs ? 1U : 0U),
                         static_cast<u8>(build.deformation), static_cast<u8>(build.surface),
                         static_cast<u8>(build.tangents)};
    for (const u8 value : flags) {
        if (Status written = writer.u8v(value); !written) {
            return written;
        }
    }
    if (Status written = writer.pad(3); !written) {
        return written;
    }
    const u32 figures[5] = {build.source_triangles, build.levels, build.resident_bytes,
                            build.resident_pages, static_cast<u32>(payload.size())};
    for (const u32 value : figures) {
        if (Status written = writer.u32v(value); !written) {
            return written;
        }
    }
    if (Status written = writer.pad(kHeaderBytes - static_cast<u32>(writer.size())); !written) {
        return written;
    }

    for (u32 index = 0; index < build.clusters.size(); ++index) {
        if (Status written =
                write_cluster_record(writer, build.clusters[index], page_of_cluster[index]);
            !written) {
            return written;
        }
    }
    for (const PageDescription& page : build.pages) {
        const usize start = writer.size();
        if (Status written = writer.u32v(page.first_cluster); !written) {
            return written;
        }
        if (Status written = writer.u32v(page.cluster_count); !written) {
            return written;
        }
        if (Status written = writer.u32v(page.byte_offset); !written) {
            return written;
        }
        if (Status written = writer.u32v(page.byte_size); !written) {
            return written;
        }
        if (Status written = writer.u8v(page.max_level); !written) {
            return written;
        }
        if (Status written = writer.u8v(static_cast<u8>(page.resident ? 1U : 0U)); !written) {
            return written;
        }
        if (Status written = writer.pad(2); !written) {
            return written;
        }
        for (const u8 byte : page.content_hash) {
            if (Status written = writer.u8v(byte); !written) {
                return written;
            }
        }
        if (Status written = writer.pad(kPageRecordBytes - static_cast<u32>(writer.size() - start));
            !written) {
            return written;
        }
    }
    for (const Group& group : build.groups) {
        const usize start = writer.size();
        const u32 fields[5] = {group.level, group.first_member, group.member_count,
                               group.first_child, group.child_count};
        for (const u32 value : fields) {
            if (Status written = writer.u32v(value); !written) {
                return written;
            }
        }
        if (Status written = writer.f32v(group.error); !written) {
            return written;
        }
        if (Status written = writer.vec3(group.sphere.center); !written) {
            return written;
        }
        if (Status written = writer.f32v(group.sphere.radius); !written) {
            return written;
        }
        if (Status written = writer.u32v(group.locked_vertices); !written) {
            return written;
        }
        if (Status written =
                writer.pad(kGroupRecordBytes - static_cast<u32>(writer.size() - start));
            !written) {
            return written;
        }
    }
    for (const u32 value : build.group_members) {
        if (Status written = writer.u32v(value); !written) {
            return written;
        }
    }
    for (const u32 value : build.group_children) {
        if (Status written = writer.u32v(value); !written) {
            return written;
        }
    }
    for (const u32 value : build.cluster_children) {
        if (Status written = writer.u32v(value); !written) {
            return written;
        }
    }
    if (Status appended = out.append(payload.span()); !appended) {
        return appended;
    }

    build.cooked_bytes = static_cast<u32>(out.size());
    build.quantisation_error = quantisation_error;
    const u32 visible_triangles = static_cast<u32>(build.indices.size() / 3);
    build.bytes_per_triangle = visible_triangles == 0 ? 0.0F
                                                      : static_cast<f32>(build.cooked_bytes) /
                                                            static_cast<f32>(visible_triangles);
    // `virtual-geometry` — "Tangent policy": "the cooker ... SHALL report the storage saved". Three
    // floats and a sign per vertex is what a stored tangent frame costs.
    build.tangent_bytes_saved = build.tangents == TangentPolicy::Stored
                                    ? 0U
                                    : static_cast<u32>(build.positions.size()) * 16U;
    return ok();
}

Expected<DecodedAsset, Error> decode_asset(Span<const u8> bytes, Allocator& allocator) noexcept {
    if (bytes.size() < kHeaderBytes) {
        return fail(ErrorCode::InvalidArgument, "decode_asset: shorter than the header");
    }
    Reader reader(bytes);
    if (reader.u32v() != kAssetMagic) {
        return fail(ErrorCode::InvalidArgument, "decode_asset: not a CYVG asset");
    }
    const u32 version = reader.u32v();
    if (version != kAssetVersion) {
        return fail(ErrorCode::Unsupported, "decode_asset: unsupported asset version");
    }

    DecodedAsset asset(allocator);
    const u32 cluster_count = reader.u32v();
    const u32 page_count = reader.u32v();
    const u32 group_count = reader.u32v();
    const u32 member_count = reader.u32v();
    const u32 child_count = reader.u32v();
    const u32 cluster_child_count = reader.u32v();
    asset.bounds.min = reader.vec3();
    asset.bounds.max = reader.vec3();
    asset.policy.min_triangles = reader.u32v();
    asset.policy.target_triangles = reader.u32v();
    asset.policy.max_triangles = reader.u32v();
    asset.policy.max_vertices = reader.u32v();
    asset.policy.group_size = reader.u32v();
    asset.policy.root_cluster_limit = reader.u32v();
    asset.policy.max_levels = reader.u32v();
    asset.policy.simplify_ratio = reader.f32v();
    asset.encoding.position_bits = reader.u32v();
    asset.encoding.normal_bits = reader.u32v();
    asset.encoding.uv_bits = reader.u32v();
    asset.encoding.has_normals = reader.u8v() != 0U;
    asset.encoding.has_uvs = reader.u8v() != 0U;
    asset.deformation = static_cast<DeformationClass>(reader.u8v());
    asset.surface = static_cast<SurfaceClass>(reader.u8v());
    asset.tangents = static_cast<TangentPolicy>(reader.u8v());
    reader.skip(3);
    asset.source_triangles = reader.u32v();
    asset.levels = reader.u32v();
    asset.resident_bytes = reader.u32v();
    reader.skip(4);  // resident_pages, recomputed from the directory below
    const u32 payload_bytes = reader.u32v();
    reader.seek(kHeaderBytes);

    const u64 needed =
        static_cast<u64>(kHeaderBytes) + (static_cast<u64>(cluster_count) * kClusterMetadataBytes) +
        (static_cast<u64>(page_count) * kPageRecordBytes) +
        (static_cast<u64>(group_count) * kGroupRecordBytes) +
        (static_cast<u64>(member_count) * 4U) + (static_cast<u64>(child_count) * 4U) +
        (static_cast<u64>(cluster_child_count) * 4U) + payload_bytes;
    if (needed != bytes.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "decode_asset: the tables and payload do not account for the whole file");
    }

    if (Status resized = asset.clusters.resize(cluster_count); !resized) {
        return make_unexpected(resized.error());
    }
    for (u32 index = 0; index < cluster_count; ++index) {
        u32 page = 0;
        read_cluster_record(reader, asset.clusters[index], page);
        asset.clusters[index].page = page;
    }
    if (Status resized = asset.pages.resize(page_count); !resized) {
        return make_unexpected(resized.error());
    }
    for (u32 index = 0; index < page_count; ++index) {
        const usize start = reader.position();
        PageDescription& page = asset.pages[index];
        page.first_cluster = reader.u32v();
        page.cluster_count = reader.u32v();
        page.byte_offset = reader.u32v();
        page.byte_size = reader.u32v();
        page.max_level = reader.u8v();
        page.resident = reader.u8v() != 0U;
        reader.skip(2);
        for (u8& byte : page.content_hash) {
            byte = reader.u8v();
        }
        reader.seek(start + kPageRecordBytes);
    }
    // The group table and the three index arrays are skipped rather than decoded: nothing in the
    // runtime reads them — traversal uses each cluster's own child range — and a reader that
    // decoded them would allocate for tables it never touches.
    reader.skip(static_cast<usize>(group_count) * kGroupRecordBytes);
    reader.skip(static_cast<usize>(member_count) * 4U);
    reader.skip(static_cast<usize>(child_count) * 4U);
    if (Status resized = asset.cluster_children.resize(cluster_child_count); !resized) {
        return make_unexpected(resized.error());
    }
    for (u32 index = 0; index < cluster_child_count; ++index) {
        asset.cluster_children[index] = reader.u32v();
    }
    if (reader.overrun()) {
        return fail(ErrorCode::InvalidArgument, "decode_asset: truncated");
    }
    asset.payload = bytes.subspan(reader.position(), payload_bytes);
    return asset;
}

namespace {

/// Where one cluster's geometry starts in the payload: after the geometry of the clusters before it
/// in its own page.
[[nodiscard]] Expected<u32, Error> cluster_payload_offset(const DecodedAsset& asset,
                                                          u32 cluster_index) noexcept {
    if (cluster_index >= asset.clusters.size()) {
        return fail(ErrorCode::OutOfRange, "no such cluster");
    }
    const Cluster& cluster = asset.clusters[cluster_index];
    if (cluster.page >= asset.pages.size()) {
        return fail(ErrorCode::InvalidArgument, "the cluster names no page");
    }
    const PageDescription& page = asset.pages[cluster.page];
    u32 offset = page.byte_offset;
    for (u32 index = page.first_cluster; index < cluster_index; ++index) {
        offset += encoded_cluster_bytes(asset.clusters[index].vertex_count,
                                        asset.clusters[index].index_count, asset.encoding) -
                  kClusterMetadataBytes;
    }
    if (offset > asset.payload.size()) {
        return fail(ErrorCode::OutOfRange, "the page offset is past the payload");
    }
    return offset;
}

}  // namespace

Expected<assets::ContentHash, Error> cluster_content_hash(const DecodedAsset& asset,
                                                          u32 cluster_index) noexcept {
    Expected<u32, Error> offset = cluster_payload_offset(asset, cluster_index);
    if (!offset) {
        return make_unexpected(offset.error());
    }
    const Cluster& cluster = asset.clusters[cluster_index];
    const u32 bytes =
        encoded_cluster_bytes(cluster.vertex_count, cluster.index_count, asset.encoding) -
        kClusterMetadataBytes;
    if (static_cast<u64>(*offset) + bytes > asset.payload.size()) {
        return fail(ErrorCode::OutOfRange, "cluster_content_hash: the payload is truncated");
    }
    // The quantisation frame is part of the content: two clusters with the same quantised vertices
    // and different bounds decode to different geometry.
    const f32 frame[10] = {cluster.bounds.min.x, cluster.bounds.min.y, cluster.bounds.min.z,
                           cluster.bounds.max.x, cluster.bounds.max.y, cluster.bounds.max.z,
                           cluster.uv_min.x,     cluster.uv_min.y,     cluster.uv_max.x,
                           cluster.uv_max.y};
    assets::ContentHasher hasher;
    hasher.update(frame, sizeof(frame));
    hasher.update(asset.payload.data() + *offset, bytes);
    return hasher.finish();
}

Expected<DecodedCluster, Error> decode_cluster(const DecodedAsset& asset, u32 cluster_index,
                                               Allocator& allocator) noexcept {
    if (cluster_index >= asset.clusters.size()) {
        return fail(ErrorCode::OutOfRange, "decode_cluster: no such cluster");
    }
    const Cluster& cluster = asset.clusters[cluster_index];
    if (cluster.page >= asset.pages.size()) {
        return fail(ErrorCode::InvalidArgument, "decode_cluster: the cluster names no page");
    }
    Expected<u32, Error> offset = cluster_payload_offset(asset, cluster_index);
    if (!offset) {
        return make_unexpected(offset.error());
    }

    Reader reader(asset.payload.subspan(*offset));
    DecodedCluster out(allocator);
    const Vec3 low = cluster.bounds.min;
    const Vec3 span{axis_span(cluster.bounds.min.x, cluster.bounds.max.x),
                    axis_span(cluster.bounds.min.y, cluster.bounds.max.y),
                    axis_span(cluster.bounds.min.z, cluster.bounds.max.z)};
    const Vec2 uv_span{axis_span(cluster.uv_min.x, cluster.uv_max.x),
                       axis_span(cluster.uv_min.y, cluster.uv_max.y)};

    for (u32 slot = 0; slot < cluster.vertex_count; ++slot) {
        const u32 x = read_component(reader, asset.encoding.position_bits);
        const u32 y = read_component(reader, asset.encoding.position_bits);
        const u32 z = read_component(reader, asset.encoding.position_bits);
        const Vec3 position{low.x + (dequantise(x, asset.encoding.position_bits) * span.x),
                            low.y + (dequantise(y, asset.encoding.position_bits) * span.y),
                            low.z + (dequantise(z, asset.encoding.position_bits) * span.z)};
        if (Status pushed = out.positions.push_back(position); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (asset.encoding.has_normals) {
            const u32 u = read_component(reader, asset.encoding.normal_bits);
            const u32 v = read_component(reader, asset.encoding.normal_bits);
            const Vec2 oct{(dequantise(u, asset.encoding.normal_bits) * 2.0F) - 1.0F,
                           (dequantise(v, asset.encoding.normal_bits) * 2.0F) - 1.0F};
            if (Status pushed = out.normals.push_back(octahedral_decode(oct)); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
        if (asset.encoding.has_uvs) {
            const u32 u = read_component(reader, asset.encoding.uv_bits);
            const u32 v = read_component(reader, asset.encoding.uv_bits);
            const Vec2 uv{cluster.uv_min.x + (dequantise(u, asset.encoding.uv_bits) * uv_span.x),
                          cluster.uv_min.y + (dequantise(v, asset.encoding.uv_bits) * uv_span.y)};
            if (Status pushed = out.uvs.push_back(uv); !pushed) {
                return make_unexpected(pushed.error());
            }
        }
    }
    for (u32 slot = 0; slot < cluster.index_count; ++slot) {
        if (Status pushed = out.indices.push_back(reader.u8v()); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    if (reader.overrun()) {
        return fail(ErrorCode::OutOfRange, "decode_cluster: the payload is truncated");
    }
    return out;
}

}  // namespace cy::rendering::vg
