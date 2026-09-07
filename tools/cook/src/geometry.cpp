#include <cy/cook/geometry.h>

namespace cy::cook {

namespace {

/// The content hash of the SOURCE the key is derived from: positions, normals, UVs, indices and the
/// per-triangle materials, framed so that two meshes that differ only in where one array ends
/// cannot hash the same. Framing here for the same reason `DerivationKeyBuilder` frames a
/// contribution: concatenating "ab" and "c" and concatenating "a" and "bc" produce the same bytes.
[[nodiscard]] assets::ContentHash hash_source(const rendering::vg::SourceMesh& mesh) noexcept {
    assets::ContentHasher hasher;
    const auto frame = [&hasher](const void* data, usize bytes) noexcept {
        const u64 length = bytes;
        hasher.update(&length, sizeof(length));
        if (bytes > 0) {
            hasher.update(data, bytes);
        }
    };
    frame(mesh.positions.data(), mesh.positions.size() * sizeof(Vec3));
    frame(mesh.normals.data(), mesh.normals.size() * sizeof(Vec3));
    frame(mesh.uvs.data(), mesh.uvs.size() * sizeof(Vec2));
    frame(mesh.indices.data(), mesh.indices.size() * sizeof(u32));
    frame(mesh.triangle_materials.data(), mesh.triangle_materials.size() * sizeof(u32));
    return hasher.finish();
}

}  // namespace

Expected<GeometryCookReport, Error> cook_virtual_geometry(const GeometryCookRequest& request,
                                                          Array<u8>& out,
                                                          Allocator& allocator) noexcept {
    GeometryCookReport report;
    report.content = hash_source(request.mesh);

    // THE KEY FIRST, so that a cook that cannot be addressed is a cook that does not run. Failing
    // afterwards would mean producing bytes nothing can cache, which is the state M6's spike found
    // and could not tell apart from a hit.
    Expected<assets::DerivationKey, Error> key =
        rendering::vg::derive_geometry_key(report.content, request.options, request.variant);
    if (!key) {
        return make_unexpected(key.error());
    }
    report.key = *key;

    Expected<rendering::vg::GeometryBuild, Error> build =
        rendering::vg::build_geometry(request.mesh, request.options, allocator);
    if (!build) {
        return make_unexpected(build.error());
    }

    // `virtual-geometry`: the check runs at COOK time, not at test time.
    Expected<rendering::vg::WatertightReport, Error> watertight =
        rendering::vg::check_watertight(*build, allocator);
    if (!watertight) {
        return make_unexpected(watertight.error());
    }
    report.watertight = watertight->watertight();
    report.closed_source = watertight->closed_source;
    report.thresholds_tested = watertight->thresholds_tested;
    if (request.fail_on_cracks && !report.watertight) {
        return fail(
            ErrorCode::Internal,
            "cook_virtual_geometry: the cluster hierarchy's cut is not watertight — some "
            "threshold selects a set of clusters that does not tile the surface, which is a "
            "hole in a frame rather than a defect a golden image would reliably catch");
    }

    rendering::vg::VertexEncoding encoding;
    encoding.position_bits = request.options.position_bits;
    encoding.normal_bits = request.options.normal_bits;
    encoding.uv_bits = request.options.uv_bits;
    encoding.has_normals = !request.mesh.normals.empty();
    encoding.has_uvs = !request.mesh.uvs.empty();
    if (Status encoded = rendering::vg::encode_asset(*build, encoding, out); !encoded) {
        return make_unexpected(encoded.error());
    }

    report.source_triangles = build->source_triangles;
    report.clusters = static_cast<u32>(build->clusters.size());
    report.pages = static_cast<u32>(build->pages.size());
    report.levels = build->levels;
    report.cooked_bytes = build->cooked_bytes;
    report.resident_bytes = build->resident_bytes;
    report.resident_pages = build->resident_pages;
    report.cluster_metadata_bytes = build->cluster_metadata_bytes;
    report.tangent_bytes_saved = build->tangent_bytes_saved;
    report.bytes_per_triangle = build->bytes_per_triangle;
    report.quantisation_error = build->quantisation_error;
    report.suitability_warning = build->suitability_warning;
    report.suitability_reason = build->suitability_reason;
    return report;
}

}  // namespace cy::cook
