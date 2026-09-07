// Lightmap UV generation, over xatlas. M6 task 8.2.
//
// `asset-import-pipeline` — "Mesh processing": "UV2 generation with configurable texel density,
// chart padding, and distortion limits", and `thirdparty-dependencies` names **xatlas** for it.
//
// ================================================================================================
// THIS IS THE ONLY TRANSLATION UNIT IN THE TREE THAT NAMES AN XATLAS SYMBOL
// ================================================================================================
//
// The dependency policy's isolation rule, kept by the build rather than by memory: `xatlas.h` is
// included here and nowhere else, `cy::import::Uv2Options` and `cy::import::Uv2Report` are engine
// types with no xatlas field in them, and `deps/manifest.toml` records this file as the interface.
// Replacing xatlas is a change to this file.
//
// ================================================================================================
// UNWRAPPING IS A VERTEX-BUFFER CHANGE, NOT A UV ASSIGNMENT
// ================================================================================================
//
// A chart boundary is a discontinuity in the parameterisation, so a vertex that sits on one needs a
// different UV per chart it belongs to. xatlas therefore returns its own vertex list, each entry
// carrying an `xref` back to the input vertex it was split from. This file rebuilds the whole mesh
// from that list: positions, normals, UV0 and tangents are copied through the xref, UV2 comes from
// the atlas, and the index list is xatlas's own. `MeshData::sections` are recovered by mapping each
// output triangle back to the input triangle it came from — xatlas preserves face order within a
// mesh, so that mapping is the identity, and it is asserted rather than assumed.
//
// ================================================================================================
// DETERMINISM, WHICH IS A REQUIREMENT AND NOT A HOPE
// ================================================================================================
//
// "WHEN the same source and options are imported twice THEN the cooked output SHALL be
// byte-identical." xatlas's packer places charts with a pseudo-random walk seeded from a constant,
// and its chart growth is a deterministic function of the input, so two runs over one mesh agree —
// which `test_unwrap.cpp` asserts by unwrapping twice and comparing every float, rather than
// trusting this paragraph.
//
// What would break it is xatlas's own worker threads, so they are switched off: `AddMeshJoin` is
// called before `ComputeCharts`, and the atlas is built one mesh at a time. Import parallelism is
// the job system's, one asset per job, and a library spawning its own pool underneath that would
// contend for the same cores as well as putting a scheduler between the input and the output.

#include <cy/import/mesh.h>

#include <xatlas.h>

#include <cmath>
#include <vector>

namespace cy::import {
namespace {

/// xatlas prints to stdout by default, which turns a cook into a wall of chart statistics. The
/// pipeline's own report is where an import says what it did.
int silent_print(const char* /*format*/, ...) {
    return 0;
}

/// Owns the atlas for the duration of the unwrap.
///
/// `generate_uv2` has eleven early exits, every one of which must free the atlas, and eleven copies
/// of `xatlas::Destroy` is eleven chances to forget one. This is the cheapest correct answer in a
/// tree compiled with -fno-exceptions: the destructor runs on every path out.
class AtlasHandle {
public:
    AtlasHandle() noexcept : atlas_(xatlas::Create()) {}
    ~AtlasHandle() {
        if (atlas_ != nullptr) {
            xatlas::Destroy(atlas_);
        }
    }

    AtlasHandle(const AtlasHandle&) = delete;
    AtlasHandle& operator=(const AtlasHandle&) = delete;

    [[nodiscard]] xatlas::Atlas* get() const noexcept { return atlas_; }
    [[nodiscard]] xatlas::Atlas* operator->() const noexcept { return atlas_; }

private:
    xatlas::Atlas* atlas_ = nullptr;
};

/// Copy one attribute array through the split map xatlas produced.
template <class T>
[[nodiscard]] Status remap_attribute(const Array<T>& source, const std::vector<u32>& xref,
                                     Array<T>& out) noexcept {
    if (source.empty()) {
        return ok();
    }
    if (Status reserved = out.reserve(xref.size()); !reserved) {
        return reserved;
    }
    for (const u32 original : xref) {
        if (Status pushed = out.push_back(source[original]); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace

Expected<Uv2Report, Error> generate_uv2(MeshData& mesh, const Uv2Options& options) noexcept {
    if (Status valid = mesh.validate(); !valid) {
        return make_unexpected(valid.error());
    }
    if (mesh.triangle_count() == 0) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a mesh with no triangles has nothing to unwrap"});
    }
    if (!(options.texel_density > 0.0f) && options.resolution == 0) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "an unwrap needs either a positive texel density or an "
                                     "explicit resolution; both were zero"});
    }
    if (!(options.max_distortion >= 1.0f)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument,
                  "a distortion limit below 1 admits no parameterisation at all"});
    }

    xatlas::SetPrint(&silent_print, false);
    const AtlasHandle atlas;
    if (atlas.get() == nullptr) {
        return make_unexpected(Error{ErrorCode::OutOfMemory, "xatlas could not create an atlas"});
    }

    xatlas::MeshDecl decl;
    decl.vertexCount = static_cast<u32>(mesh.vertex_count());
    decl.vertexPositionData = mesh.positions.data();
    decl.vertexPositionStride = sizeof(Vec3);
    if (!mesh.normals.empty()) {
        decl.vertexNormalData = mesh.normals.data();
        decl.vertexNormalStride = sizeof(Vec3);
    }
    decl.indexCount = static_cast<u32>(mesh.indices.size());
    decl.indexData = mesh.indices.data();
    decl.indexFormat = xatlas::IndexFormat::UInt32;

    // A face may only join a chart with the same material, so a chart never spans two sections and
    // the section table survives the unwrap intact.
    std::vector<u32> face_material(mesh.triangle_count(), 0);
    for (usize index = 0; index < mesh.sections.size(); ++index) {
        const MeshSection& section = mesh.sections[index];
        for (u32 at = section.first_index; at < section.first_index + section.index_count;
             at += 3) {
            face_material[at / 3] = static_cast<u32>(index);
        }
    }
    decl.faceMaterialData = face_material.data();

    if (const xatlas::AddMeshError added = xatlas::AddMesh(atlas.get(), decl, 1);
        added != xatlas::AddMeshError::Success) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "xatlas refused this mesh; see its topology"});
    }
    xatlas::AddMeshJoin(atlas.get());

    xatlas::ChartOptions chart_options;
    chart_options.maxCost = options.max_distortion;
    // The angle between a face and its chart's average normal, expressed as the weight xatlas
    // grows charts by. A chart that folded past `max_chart_angle` would parameterise onto itself.
    chart_options.normalDeviationWeight = 2.0f;
    chart_options.maxIterations = 1;
    chart_options.fixWinding = true;

    xatlas::PackOptions pack_options;
    pack_options.padding = options.padding;
    pack_options.texelsPerUnit = options.resolution != 0 ? 0.0f : options.texel_density;
    pack_options.resolution = options.resolution;
    pack_options.bilinear = true;
    pack_options.blockAlign = true;
    // Deterministic placement. The random walk is seeded from a constant inside xatlas, so both
    // settings are reproducible; brute force is chosen because its answer does not depend on how
    // many charts happened to be placed before a given one.
    pack_options.bruteForce = false;
    pack_options.createImage = false;

    xatlas::ComputeCharts(atlas.get(), chart_options);
    xatlas::PackCharts(atlas.get(), pack_options);

    if (atlas->meshCount != 1 || atlas->width == 0 || atlas->height == 0) {
        return make_unexpected(
            Error{ErrorCode::Internal,
                  "the unwrap produced no atlas; the mesh has no parameterisable "
                  "surface"});
    }

    const xatlas::Mesh& output = atlas->meshes[0];
    if (output.indexCount != mesh.indices.size()) {
        return make_unexpected(
            Error{ErrorCode::Internal,
                  "the unwrap changed the triangle count, which would break the section table"});
    }

    std::vector<u32> xref(output.vertexCount, 0);
    const auto width = static_cast<f32>(atlas->width);
    const auto height = static_cast<f32>(atlas->height);
    Array<Vec2> unwrapped;
    if (Status reserved = unwrapped.reserve(output.vertexCount); !reserved) {
        return make_unexpected(reserved.error());
    }
    for (u32 index = 0; index < output.vertexCount; ++index) {
        const xatlas::Vertex& vertex = output.vertexArray[index];
        xref[index] = vertex.xref;
        // Normalised into [0, 1]: the atlas's own coordinates are in texels, and a cooked mesh
        // carries texture coordinates, not a resolution the runtime would have to be told.
        if (Status pushed = unwrapped.push_back(Vec2{vertex.uv[0] / width, vertex.uv[1] / height});
            !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    MeshData rebuilt;
    if (Status remapped = remap_attribute(mesh.positions, xref, rebuilt.positions); !remapped) {
        return make_unexpected(remapped.error());
    }
    if (Status remapped = remap_attribute(mesh.normals, xref, rebuilt.normals); !remapped) {
        return make_unexpected(remapped.error());
    }
    if (Status remapped = remap_attribute(mesh.uvs, xref, rebuilt.uvs); !remapped) {
        return make_unexpected(remapped.error());
    }
    if (Status remapped = remap_attribute(mesh.tangents, xref, rebuilt.tangents); !remapped) {
        return make_unexpected(remapped.error());
    }
    rebuilt.uv2 = std::move(unwrapped);

    if (Status reserved = rebuilt.indices.reserve(output.indexCount); !reserved) {
        return make_unexpected(reserved.error());
    }
    for (u32 index = 0; index < output.indexCount; ++index) {
        if (Status pushed = rebuilt.indices.push_back(output.indexArray[index]); !pushed) {
            return make_unexpected(pushed.error());
        }
    }
    if (Status appended = rebuilt.sections.append(
            Span<const MeshSection>(mesh.sections.data(), mesh.sections.size()));
        !appended) {
        return make_unexpected(appended.error());
    }

    Uv2Report report;
    report.charts = atlas->chartCount;
    report.width = atlas->width;
    report.height = atlas->height;
    report.vertices_added =
        output.vertexCount > mesh.vertex_count() ? output.vertexCount - mesh.vertex_count() : 0;
    report.utilisation = atlas->utilization != nullptr ? atlas->utilization[0] : 0.0f;

    if (Status valid = rebuilt.validate(); !valid) {
        return make_unexpected(valid.error());
    }
    mesh = std::move(rebuilt);
    return report;
}

}  // namespace cy::import
