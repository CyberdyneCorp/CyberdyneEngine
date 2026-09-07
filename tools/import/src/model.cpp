#include <cy/import/model.h>

#include <cstring>
#include <string>

namespace cy::import {
namespace {

/// Copy a mesh. `MeshData` is move-only by construction — its arrays are the engine's, whose copy
/// is named and fallible — and the cooked form is a byte-exact round trip that already exists, so
/// this is a copy through the format rather than a second copy routine that could disagree with it.
[[nodiscard]] Status clone_mesh(const MeshData& source, MeshData& out) noexcept {
    Array<u8> payload;
    if (Status written = write_cooked_mesh(source, payload); !written) {
        return written;
    }
    return read_cooked_mesh(Span<const u8>(payload.data(), payload.size()), out);
}

}  // namespace

std::string SubAssetNames::unique(std::string_view prefix, std::string_view name,
                                  usize fallback_index) {
    std::string candidate(prefix);
    if (name.empty()) {
        candidate += "unnamed-";
        candidate += std::to_string(fallback_index);
    } else {
        candidate += name;
    }
    std::string attempt = candidate;
    usize suffix = 1;
    while (true) {
        bool clash = false;
        for (const std::string& existing : taken_) {
            clash = clash || existing == attempt;
        }
        if (!clash) {
            taken_.push_back(attempt);
            return attempt;
        }
        attempt = candidate + "." + std::to_string(suffix++);
    }
}

bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return suffix.size() <= text.size() && text.substr(text.size() - suffix.size()) == suffix;
}

Status finish_mesh(MeshData& mesh, const ModelBuildOptions& options, ImportResult& out,
                   std::string_view subject) noexcept {
    if (mesh.normals.empty()) {
        if (Status generated = generate_normals(mesh, options.smoothing_angle); !generated) {
            return generated;
        }
    }
    WeldOptions weld_options;
    weld_options.position_tolerance = options.weld_tolerance;
    if (Expected<usize, Error> merged = weld(mesh, weld_options); !merged) {
        return make_unexpected(merged.error());
    }
    if (options.generate_tangent_basis && !mesh.uvs.empty()) {
        if (Status generated = generate_tangents(mesh); !generated) {
            return generated;
        }
    }

    if (options.generate_lightmap_uvs) {
        Expected<Uv2Report, Error> unwrapped = generate_uv2(mesh, options.uv2);
        if (!unwrapped) {
            // An unwrap that fails is a fact about the mesh — a surface with no parameterisation, a
            // density that will not fit — and not a reason to lose the mesh. The lightmap
            // coordinates are simply absent, which the runtime already handles: `MeshAttributes`
            // records which arrays a cooked mesh carries.
            if (Status reported = out.report(ImportSeverity::Warning, "unwrap-failed",
                                             unwrapped.error().message, subject);
                !reported) {
                return reported;
            }
        } else if (Status reported = out.report(
                       ImportSeverity::Info, "unwrapped",
                       "lightmap coordinates were generated; the vertex buffer grew by the "
                       "vertices the chart boundaries split",
                       subject);
                   !reported) {
            return reported;
        }
        // Tangents are per-vertex and the unwrap renumbered the vertices, so a basis generated
        // before it has been carried through the split correctly by the remap — but a mesh that had
        // none and gained UVs still has none. Nothing to do either way; stated so the omission
        // reads as a decision.
    }

    if (options.optimise) {
        if (Status ordered = optimise_vertex_cache(mesh); !ordered) {
            return ordered;
        }
        if (options.overdraw_threshold > 1.0f) {
            if (Status ordered = optimise_overdraw(mesh, options.overdraw_threshold); !ordered) {
                return ordered;
            }
        }
        if (Status ordered = optimise_vertex_fetch(mesh); !ordered) {
            return ordered;
        }
    }
    return ok();
}

Status emit_mesh_with_lods(const MeshData& mesh, std::string_view name,
                           const ModelBuildOptions& options, ImportResult& out) noexcept {
    Array<u8> payload;
    if (Status written = write_cooked_mesh(mesh, payload); !written) {
        return written;
    }
    if (Status added = out.add(assets::AssetKind::Mesh, name, std::move(payload), false); !added) {
        return added;
    }

    MeshData level;
    for (i64 lod = 1; lod <= options.lod_count; ++lod) {
        if (lod == 1) {
            if (Status copied = clone_mesh(mesh, level); !copied) {
                return copied;
            }
        }
        SimplifyOptions simplify_options;
        simplify_options.target_ratio = options.lod_ratio;
        simplify_options.error_bound = options.lod_error_bound;
        Expected<SimplifyReport, Error> reduced = simplify(level, simplify_options);
        if (!reduced) {
            return make_unexpected(reduced.error());
        }
        if (reduced.value().bounded) {
            if (Status reported = out.report(
                    ImportSeverity::Info, "lod-error-bound",
                    "a level of detail stopped short of its target because the next collapse "
                    "exceeded the error bound; the level is larger rather than damaged",
                    name);
                !reported) {
                return reported;
            }
        }
        Array<u8> lod_payload;
        if (Status written = write_cooked_mesh(level, lod_payload); !written) {
            return written;
        }
        std::string lod_name(name);
        lod_name += "/lod";
        lod_name += std::to_string(lod);
        if (Status added =
                out.add(assets::AssetKind::Mesh, lod_name, std::move(lod_payload), false);
            !added) {
            return added;
        }
    }
    return ok();
}

namespace {

/// Strip everything a solver does not read. A collider carries positions and topology: normals,
/// texture coordinates and tangents are render data, and shipping them would double a collision
/// asset for information nothing consumes.
void strip_render_attributes(MeshData& collider) noexcept {
    collider.normals.clear();
    collider.uvs.clear();
    collider.uv2.clear();
    collider.tangents.clear();
    collider.sections.clear();
}

}  // namespace

namespace {

/// The `decompose` mode: a bounded set of hulls, emitted as `<name>/part<n>`.
///
/// Returns how many parts were emitted, and zero when the decomposition itself failed — which is a
/// fact about the shape rather than an error, and which `emit_collision` answers by falling back to
/// a single hull.
[[nodiscard]] Expected<usize, Error> emit_decomposition(const MeshData& source,
                                                        std::string_view name,
                                                        ImportResult& out) noexcept {
    const ConvexDecompositionOptions options;
    std::vector<MeshData> parts;
    Expected<usize, Error> produced = convex_decomposition(source, options, parts);
    if (!produced) {
        if (Status reported = out.report(ImportSeverity::Warning, "collision-decomposition-failed",
                                         produced.error().message, name);
            !reported) {
            return make_unexpected(reported.error());
        }
        return usize{0};
    }
    for (usize index = 0; index < parts.size(); ++index) {
        strip_render_attributes(parts[index]);
        Array<u8> payload;
        if (Status written = write_cooked_mesh(parts[index], payload); !written) {
            return make_unexpected(written.error());
        }
        std::string part_name(name);
        part_name += "/part";
        part_name += std::to_string(index);
        if (Status added = out.add(assets::AssetKind::Mesh, part_name, std::move(payload), false);
            !added) {
            return make_unexpected(added.error());
        }
    }
    return parts.size();
}

/// The `convex` and `triangle` modes, and the fallback when a hull cannot be built.
[[nodiscard]] Status build_single_collider(const MeshData& source, std::string_view name,
                                           const ModelBuildOptions& options, ImportResult& out,
                                           MeshData& collider) noexcept {
    if (options.collision_mode == "convex") {
        if (Status hulled = convex_hull(source, collider); !hulled) {
            if (Status reported = out.report(
                    ImportSeverity::Warning, "collision-hull-failed",
                    "a convex hull could not be built from this node — it is flat or has fewer "
                    "than four points — so its triangles were kept as they are",
                    name);
                !reported) {
                return reported;
            }
            collider.clear();
        }
    }
    if (collider.indices.empty()) {
        if (Status copied = clone_mesh(source, collider); !copied) {
            return copied;
        }
    }
    strip_render_attributes(collider);
    return ok();
}

}  // namespace

Expected<usize, Error> emit_collision(const MeshData& source, std::string_view name,
                                      const ModelBuildOptions& options,
                                      ImportResult& out) noexcept {
    if (options.collision_mode == "none" || source.indices.empty()) {
        return usize{0};
    }
    if (options.collision_mode == "decompose") {
        Expected<usize, Error> parts = emit_decomposition(source, name, out);
        if (!parts) {
            return parts;
        }
        if (parts.value() != 0) {
            return parts;
        }
        // The decomposition could not run. Fall through to a single hull rather than leaving the
        // node with no collision at all.
    }

    MeshData collider;
    if (Status built = build_single_collider(source, name, options, out, collider); !built) {
        return make_unexpected(built.error());
    }
    Array<u8> payload;
    if (Status written = write_cooked_mesh(collider, payload); !written) {
        return make_unexpected(written.error());
    }
    if (Status added = out.add(assets::AssetKind::Mesh, name, std::move(payload), false); !added) {
        return make_unexpected(added.error());
    }
    return usize{1};
}

namespace {

void put_u32(Array<u8>& out, u32 value) noexcept {
    for (u32 shift = 0; shift < 4; ++shift) {
        (void)out.push_back(static_cast<u8>((value >> (shift * 8U)) & 0xFFU));
    }
}

void put_f32(Array<u8>& out, f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

[[nodiscard]] u32 get_u32(const u8* data) noexcept {
    u32 value = 0;
    for (u32 index = 0; index < 4; ++index) {
        value |= static_cast<u32>(data[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] f32 get_f32(const u8* data) noexcept {
    const u32 bits = get_u32(data);
    f32 value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

/// The record is 13 little-endian 32-bit words: the version, four base colour lanes, metallic,
/// roughness, three emissive lanes, the alpha mode, the cutoff and the two-sided flag.
constexpr usize kMaterialRecordBytes = usize{13} * 4;

}  // namespace

Status write_cooked_material(const StandardMaterial& material, Array<u8>& out) noexcept {
    if (Status reserved = out.reserve(out.size() + kMaterialRecordBytes); !reserved) {
        return reserved;
    }
    put_u32(out, kCookedMaterialVersion);
    for (const f32 lane : material.base_colour) {
        put_f32(out, lane);
    }
    put_f32(out, material.metallic);
    put_f32(out, material.roughness);
    for (const f32 lane : material.emissive) {
        put_f32(out, lane);
    }
    put_u32(out, material.alpha_mode);
    put_f32(out, material.alpha_cutoff);
    put_u32(out, material.double_sided ? 1U : 0U);
    return ok();
}

Status read_cooked_material(Span<const u8> payload, StandardMaterial& out) noexcept {
    if (payload.size() < kMaterialRecordBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked material record is shorter than a header");
    }
    const u8* at = payload.data();
    if (get_u32(at) != kCookedMaterialVersion) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked material record at a version this build does not read");
    }
    at += 4;
    for (f32& lane : out.base_colour) {
        lane = get_f32(at);
        at += 4;
    }
    out.metallic = get_f32(at);
    at += 4;
    out.roughness = get_f32(at);
    at += 4;
    for (f32& lane : out.emissive) {
        lane = get_f32(at);
        at += 4;
    }
    out.alpha_mode = get_u32(at);
    at += 4;
    out.alpha_cutoff = get_f32(at);
    at += 4;
    out.double_sided = get_u32(at) != 0U;
    return ok();
}

Status emit_prefab(Span<const ImportedNode> nodes, ImportResult& out) noexcept {
    Array<u8> graph;
    if (Status written = write_cooked_scene_graph(nodes, graph); !written) {
        return written;
    }
    return out.add(assets::AssetKind::Prefab, "prefab", std::move(graph), true);
}

}  // namespace cy::import
