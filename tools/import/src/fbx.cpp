#include <cy/import/fbx.h>

#include <cy/import/model.h>

#include <ufbx.h>

#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace cy::import {
namespace {

// --- Options -------------------------------------------------------------------------------------
//
// The names are the glTF schema's, deliberately. See fbx.h.

constexpr std::string_view kUpAxisChoices[] = {"auto", "y-up", "z-up"};
constexpr std::string_view kCollisionChoices[] = {"none", "convex", "decompose", "triangle"};

constexpr OptionSpec kFbxOptions[] = {
    {"scale",
     OptionType::Float,
     OptionValue::of_float(1.0),
     "A uniform scale applied after the file's own unit conversion. An FBX declares its unit and "
     "ufbx converts it to metres, so this is the escape hatch for a file whose declaration is "
     "wrong rather than the normal way to rescale a model.",
     {},
     1.0e-6,
     1.0e6},
    {"source-up-axis", OptionType::Enumeration, OptionValue::of_enumeration("auto"),
     "Which axis is up in the source file. 'auto' trusts the file's own declaration, which is "
     "right for every correctly written FBX; the other two override it for a file whose exporter "
     "wrote the wrong axis system, which is common enough to need a switch.",
     Span<const std::string_view>(kUpAxisChoices), 0.0, 0.0},
    {"import-meshes",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to produce meshes. Turning it off is the fast path for iterating on materials or on "
     "the hierarchy alone.",
     {},
     0.0,
     0.0},
    {"import-materials",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to produce material sub-assets from the source's own materials.",
     {},
     0.0,
     0.0},
    {"weld-tolerance",
     OptionType::Float,
     OptionValue::of_float(1.0e-5),
     "How close two positions must be, in metres, to be merged into one vertex. FBX stores "
     "attributes per corner rather than per vertex, so this step is what turns a de-indexed face "
     "list back into a vertex buffer.",
     {},
     0.0,
     1.0},
    {"smoothing-angle",
     OptionType::Float,
     OptionValue::of_float(60.0),
     "The angle in degrees beyond which two faces are a hard edge, used when the source supplies "
     "no normals. 180 makes everything smooth; 0 makes everything flat.",
     {},
     0.0,
     180.0},
    {"generate-tangents",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to generate a tangent basis where the source has none. Needed by every normal-mapped "
     "material.",
     {},
     0.0,
     0.0},
    {"optimise",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to reorder triangles for the post-transform vertex cache and for overdraw, and "
     "vertices for fetch locality. Off only to compare against an unoptimised import.",
     {},
     0.0,
     0.0},
    {"overdraw-threshold",
     OptionType::Float,
     OptionValue::of_float(1.05),
     "How much of the vertex cache's efficiency the overdraw reorder may spend, as a ratio. 1 "
     "forbids any regression and disables the step in practice.",
     {},
     1.0,
     4.0},
    {"generate-lightmap-uvs",
     OptionType::Bool,
     OptionValue::of_bool(false),
     "Whether to unwrap a second texture coordinate set for lightmapping. It costs an unwrap per "
     "mesh and splits vertices at every chart boundary, so a project that bakes no lightmaps "
     "should leave it off.",
     {},
     0.0,
     0.0},
    {"lightmap-texel-density",
     OptionType::Float,
     OptionValue::of_float(16.0),
     "Texels per world unit for the generated lightmap atlas. The atlas is sized from this and "
     "from the mesh's own surface area, so one number describes a whole project.",
     {},
     0.25,
     1024.0},
    {"lightmap-padding",
     OptionType::Int,
     OptionValue::of_int(2),
     "Texels left between charts so a bilinear tap at a chart's edge cannot reach its neighbour.",
     {},
     0.0,
     32.0},
    {"lod-count",
     OptionType::Int,
     OptionValue::of_int(0),
     "How many reduced levels of detail to generate after the full-detail mesh. Zero produces "
     "none.",
     {},
     0.0,
     8.0},
    {"lod-ratio",
     OptionType::Float,
     OptionValue::of_float(0.5),
     "The share of the previous level's triangles each level of detail keeps.",
     {},
     0.05,
     0.95},
    {"lod-error-bound",
     OptionType::Float,
     OptionValue::of_float(0.0),
     "Stop simplifying a level before a collapse whose squared error exceeds this, so a mesh that "
     "cannot be reduced safely comes back larger rather than damaged. Zero means no bound.",
     {},
     0.0,
     1.0e6},
    {"collision-suffix",
     OptionType::Text,
     OptionValue::of_text("_collision"),
     "A node whose name ends with this becomes a collider and is excluded from rendering. Empty "
     "disables the convention. FBX pipelines conventionally use UCX_ or _collision; this is the "
     "suffix form, and a project with the prefix convention sets it empty and names its nodes.",
     {},
     0.0,
     0.0},
    {"collision-mode", OptionType::Enumeration, OptionValue::of_enumeration("convex"),
     "What a collision node produces: nothing, one convex hull, a bounded set of hulls from a "
     "convex decomposition, or the triangle mesh as it stands.",
     Span<const std::string_view>(kCollisionChoices), 0.0, 0.0},
};

constexpr std::string_view kExtensions[] = {".fbx"};
constexpr assets::AssetKind kProduces[] = {assets::AssetKind::Mesh, assets::AssetKind::Material,
                                           assets::AssetKind::Prefab};

// --- ufbx glue -----------------------------------------------------------------------------------

/// Everything read out of the option schema, so `import` reads as the ordered list of steps.
struct FbxState {
    OptionsSchema options;
    f32 scale = 1.0f;
    /// Empty for `auto`. Otherwise "y-up" or "z-up", and the conversion is this importer's rather
    /// than ufbx's.
    std::string_view up_override;
    bool import_meshes = true;
    bool import_materials = true;
    ModelBuildOptions build;
};

[[nodiscard]] std::string_view view_of(const ufbx_string& text) noexcept {
    return {text.data, text.length};
}

[[nodiscard]] Vec3 to_vec3(const ufbx_vec3& value) noexcept {
    return Vec3{static_cast<f32>(value.x), static_cast<f32>(value.y), static_cast<f32>(value.z)};
}

/// The rotation that sends a Z-up frame to the engine's Y-up one: -90 degrees about X, which sends
/// +Z to +Y and +Y to -Z. It is the same rotation `gltf.cpp` applies, expressed as a quaternion
/// because of WHERE this importer applies it — see below.
[[nodiscard]] Quat z_up_to_y_up() noexcept {
    return Quat::from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, -1.5707963268f);
}

/// THE OVERRIDE IS ONE ROTATION AT THE ROOT, NOT A ROTATION PER VERTEX.
///
/// A change of coordinate frame is a single rigid transform applied to the whole scene, so applying
/// it to the ROOT nodes and letting the hierarchy carry it down is both correct and the only form
/// that is correct: rotating every vertex while leaving the node transforms alone gets a flat
/// hierarchy right and every rotated node wrong, which is the bug that hides until a project
/// imports a model whose parts are placed rather than modelled in position.
///
/// `auto` — the default and the ordinary case — never reaches this: ufbx has already put the scene
/// into the engine's frame from the file's own declaration, and its conversion lands in exactly the
/// same place, the node transforms.
void apply_root_override(const FbxState& state, ImportedNode& node) noexcept {
    if (state.up_override != "z-up" || node.parent != -1) {
        return;
    }
    const Quat rotation = z_up_to_y_up();
    node.translation = rotation * node.translation;
    node.rotation = rotation * node.rotation;
}

/// Which attribute arrays an FBX mesh supplies, resolved once rather than per corner.
struct SourceAttributes {
    bool normals = false;
    bool uvs = false;
};

/// Append one corner of one triangle as a vertex, with whatever attributes the source carries.
///
/// FBX stores attributes PER CORNER: a cube's eight positions become twenty-four index entries,
/// each with its own normal and texture coordinate. So the natural conversion is one vertex per
/// corner, and `finish_mesh`'s weld is what brings it back — the same welder the glTF path runs, so
/// a cube exported to both formats produces one vertex count rather than two.
[[nodiscard]] Status append_corner(const FbxState& state, const ufbx_mesh& source,
                                   const SourceAttributes& attributes, u32 index,
                                   MeshData& out) noexcept {
    const auto vertex = static_cast<u32>(out.positions.size());
    if (Status pushed = out.positions.push_back(
            to_vec3(ufbx_get_vertex_vec3(&source.vertex_position, index)) * state.scale);
        !pushed) {
        return pushed;
    }
    if (attributes.normals) {
        if (Status pushed =
                out.normals.push_back(to_vec3(ufbx_get_vertex_vec3(&source.vertex_normal, index)));
            !pushed) {
            return pushed;
        }
    }
    if (attributes.uvs) {
        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&source.vertex_uv, index);
        if (Status pushed = out.uvs.push_back(Vec2{static_cast<f32>(uv.x), static_cast<f32>(uv.y)});
            !pushed) {
            return pushed;
        }
    }
    return out.indices.push_back(vertex);
}

/// Append one material part's faces, triangulated, as a run of indices.
[[nodiscard]] Status append_part(const FbxState& state, const ufbx_mesh& source,
                                 const SourceAttributes& attributes, const ufbx_mesh_part* part,
                                 std::vector<u32>& corners, MeshData& out) noexcept {
    const usize face_count = part != nullptr ? part->num_faces : source.num_faces;
    for (usize at = 0; at < face_count; ++at) {
        const u32 face_index = part != nullptr ? part->face_indices.data[at] : static_cast<u32>(at);
        const ufbx_face face = source.faces.data[face_index];
        if (face.num_indices < 3) {
            continue;
        }
        const u32 triangles = ufbx_triangulate_face(corners.data(), corners.size(), &source, face);
        for (u32 corner = 0; corner < triangles * 3; ++corner) {
            if (Status appended = append_corner(state, source, attributes, corners[corner], out);
                !appended) {
                return appended;
            }
        }
    }
    return ok();
}

/// One FBX mesh, de-indexed into the engine's parallel-array form and split into sections by
/// material.
[[nodiscard]] Status build_mesh(const FbxState& state, const ufbx_mesh& source,
                                MeshData& out) noexcept {
    SourceAttributes attributes;
    attributes.normals = source.vertex_normal.exists;
    attributes.uvs = source.vertex_uv.exists;

    // Triangulating a face needs room for its own triangles; `max_face_triangles * 3` is the bound
    // ufbx documents.
    std::vector<u32> corners((source.max_face_triangles * 3) + 3, 0);

    // One section per material, in material order, so two imports of one file agree. A mesh with no
    // material list still gets one section covering everything, because a cooked mesh addresses its
    // draws through sections and a mesh with none would draw nothing.
    const usize part_count = source.material_parts.count > 0 ? source.material_parts.count : 1;
    for (usize part_index = 0; part_index < part_count; ++part_index) {
        const auto first_index = static_cast<u32>(out.indices.size());
        const ufbx_mesh_part* part =
            source.material_parts.count > 0 ? &source.material_parts.data[part_index] : nullptr;
        if (Status appended = append_part(state, source, attributes, part, corners, out);
            !appended) {
            return appended;
        }

        MeshSection section;
        section.first_index = first_index;
        section.index_count = static_cast<u32>(out.indices.size()) - first_index;
        section.material = part != nullptr ? part->index : 0;
        if (section.index_count == 0) {
            continue;
        }
        if (Status pushed = out.sections.push_back(section); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// Map a ufbx material onto the standard material every model importer writes.
[[nodiscard]] StandardMaterial standard_material_of(const ufbx_material& source) noexcept {
    StandardMaterial material;
    const ufbx_material_pbr_maps& pbr = source.pbr;
    const auto factor =
        static_cast<f32>(pbr.base_factor.has_value ? pbr.base_factor.value_real : 1.0);
    material.base_colour[0] = static_cast<f32>(pbr.base_color.value_vec4.x) * factor;
    material.base_colour[1] = static_cast<f32>(pbr.base_color.value_vec4.y) * factor;
    material.base_colour[2] = static_cast<f32>(pbr.base_color.value_vec4.z) * factor;
    // Opacity is a separate map in FBX and the alpha lane of the base colour in the standard
    // material, which is where a masked or blended material reads it from.
    material.base_colour[3] =
        static_cast<f32>(pbr.opacity.has_value ? pbr.opacity.value_real : 1.0);
    material.metallic = static_cast<f32>(pbr.metalness.has_value ? pbr.metalness.value_real : 0.0);
    material.roughness = static_cast<f32>(pbr.roughness.has_value ? pbr.roughness.value_real : 1.0);
    const auto emission =
        static_cast<f32>(pbr.emission_factor.has_value ? pbr.emission_factor.value_real : 1.0);
    material.emissive[0] = static_cast<f32>(pbr.emission_color.value_vec3.x) * emission;
    material.emissive[1] = static_cast<f32>(pbr.emission_color.value_vec3.y) * emission;
    material.emissive[2] = static_cast<f32>(pbr.emission_color.value_vec3.z) * emission;
    // FBX has no alpha mode: a material is blended when its opacity is below one, and there is no
    // masked mode at all. Choosing `blend` from the opacity is what every FBX importer does, and
    // saying so here is better than a comment in a bug report later.
    material.alpha_mode = material.base_colour[3] < 1.0f ? 2U : 0U;
    material.double_sided = source.features.double_sided.enabled;
    return material;
}

}  // namespace

OptionsSchema fbx_options() noexcept {
    return OptionsSchema(Span<const OptionSpec>(kFbxOptions));
}

ImporterInfo FbxImporter::info() const noexcept {
    ImporterInfo info;
    info.name = "fbx";
    info.version = 1;
    info.extensions = Span<const std::string_view>(kExtensions);
    info.produces = Span<const assets::AssetKind>(kProduces);
    info.description =
        "Imports an FBX file into cooked meshes with levels of detail, standard materials, "
        "collision proxies from a naming convention, and the node hierarchy the cook step turns "
        "into a prefab. Parsing is ufbx's; every step after it is shared with the glTF importer.";
    return info;
}

OptionsSchema FbxImporter::schema() const noexcept {
    return fbx_options();
}

Status FbxImporter::import(const ImportRequest& request, ImportResult& out) noexcept {
    FbxState state;
    state.options = schema();

    const auto option = [&](std::string_view name) noexcept {
        return request.option(state.options, name);
    };
    Expected<OptionValue, Error> scale = option("scale");
    Expected<OptionValue, Error> up = option("source-up-axis");
    Expected<OptionValue, Error> meshes = option("import-meshes");
    Expected<OptionValue, Error> materials = option("import-materials");
    Expected<OptionValue, Error> weld_tolerance = option("weld-tolerance");
    Expected<OptionValue, Error> smoothing = option("smoothing-angle");
    Expected<OptionValue, Error> tangents = option("generate-tangents");
    Expected<OptionValue, Error> optimise_option = option("optimise");
    Expected<OptionValue, Error> overdraw = option("overdraw-threshold");
    Expected<OptionValue, Error> lightmap = option("generate-lightmap-uvs");
    Expected<OptionValue, Error> density = option("lightmap-texel-density");
    Expected<OptionValue, Error> padding = option("lightmap-padding");
    Expected<OptionValue, Error> lod_count = option("lod-count");
    Expected<OptionValue, Error> lod_ratio = option("lod-ratio");
    Expected<OptionValue, Error> lod_error = option("lod-error-bound");
    Expected<OptionValue, Error> collision_suffix = option("collision-suffix");
    Expected<OptionValue, Error> collision_mode = option("collision-mode");
    if (!scale || !up || !meshes || !materials || !weld_tolerance || !smoothing || !tangents ||
        !optimise_option || !overdraw || !lightmap || !density || !padding || !lod_count ||
        !lod_ratio || !lod_error || !collision_suffix || !collision_mode) {
        return fail(ErrorCode::Internal, "the FBX importer's own option schema is inconsistent");
    }
    state.scale = static_cast<f32>(scale.value().as_float());
    const std::string_view up_axis = up.value().as_text();
    state.up_override = up_axis == "auto" ? std::string_view{} : up_axis;
    state.import_meshes = meshes.value().as_bool();
    state.import_materials = materials.value().as_bool();
    state.build.weld_tolerance = static_cast<f32>(weld_tolerance.value().as_float());
    state.build.smoothing_angle = static_cast<f32>(smoothing.value().as_float());
    state.build.generate_tangent_basis = tangents.value().as_bool();
    state.build.optimise = optimise_option.value().as_bool();
    state.build.overdraw_threshold = static_cast<f32>(overdraw.value().as_float());
    state.build.generate_lightmap_uvs = lightmap.value().as_bool();
    state.build.uv2.texel_density = static_cast<f32>(density.value().as_float());
    state.build.uv2.padding = static_cast<u32>(padding.value().as_int());
    state.build.lod_count = lod_count.value().as_int();
    state.build.lod_ratio = static_cast<f32>(lod_ratio.value().as_float());
    state.build.lod_error_bound = static_cast<f32>(lod_error.value().as_float());
    state.build.collision_suffix = collision_suffix.value().as_text();
    state.build.collision_mode = collision_mode.value().as_text();

    // --- 1. Parse, and convert to the engine's conventions.
    ufbx_load_opts opts = {};
    if (state.up_override.empty()) {
        // The ordinary path: the file's own declaration is trusted and ufbx rewrites the scene into
        // the engine's right-handed, Y-up, metre space.
        //
        // `MODIFY_GEOMETRY` rather than `ADJUST_TRANSFORMS`, and the difference is not cosmetic.
        // Both put the AXIS conversion into the node transforms, which is where a change of frame
        // belongs; they differ on the UNIT conversion. `ADJUST_TRANSFORMS` leaves the geometry in
        // the file's own units and scales the transforms, so a centimetre file yields vertices in
        // centimetres under transforms in metres — two spaces in one scene, and a cooked mesh whose
        // bounds are a hundred times too large. `MODIFY_GEOMETRY` scales the geometry too, so
        // everything this importer reads is already in metres.
        opts.target_axes = ufbx_axes_right_handed_y_up;
        opts.target_unit_meters = 1.0f;
        opts.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    }
    // Normals are generated by the engine's own `generate_normals` when the file has none, so that
    // the smoothing angle is one number with one meaning across both importers. ufbx is not asked
    // to invent them.
    opts.generate_missing_normals = false;
    // The importer is handed bytes and may not open a file (`importer.h`). ufbx must not either:
    // an external texture or cache file is read through the resolver, which records it as a
    // dependency in the same act.
    opts.load_external_files = false;
    opts.ignore_missing_external_files = true;
    // A geometry transform is a non-inherited transform FBX applies to a node's attachment only.
    // Baking it into the geometry is what makes the node table a plain TRS hierarchy the engine's
    // scene graph can hold.
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    opts.clean_skin_weights = true;

    ufbx_error error = {};
    ufbx_scene* scene = ufbx_load_memory(request.bytes.data(), request.bytes.size(), &opts, &error);
    if (scene == nullptr) {
        char description[512] = {};
        (void)ufbx_format_error(description, sizeof(description), &error);
        return out.report(ImportSeverity::Error, "unreadable-fbx", description,
                          request.source.view());
    }

    // --- Textures. Recorded as dependencies and referenced by path; the texture importer is what
    // imports them. Two importers for one file would mean two cooked textures and two asset ids for
    // the same image.
    for (usize index = 0; index < scene->textures.count; ++index) {
        const ufbx_texture& texture = *scene->textures.data[index];
        if (texture.type != UFBX_TEXTURE_FILE) {
            continue;
        }
        const std::string_view relative = view_of(texture.relative_filename);
        if (relative.empty() || request.resolver == nullptr) {
            continue;
        }
        if (Expected<Span<const u8>, Error> bytes = request.resolver->read(relative); !bytes) {
            if (Status reported = out.report(
                    ImportSeverity::Warning, "missing-texture",
                    "a texture this model references is not beside it; the material will resolve "
                    "to a placeholder until it is added",
                    relative);
                !reported) {
                ufbx_free_scene(scene);
                return reported;
            }
        }
    }

    // --- Report what is read and skipped, once, rather than per mesh.
    if (scene->skin_deformers.count != 0 || scene->anim_stacks.count > 1 ||
        scene->blend_deformers.count != 0) {
        if (Status reported = out.report(
                ImportSeverity::Warning, "skipped-rig",
                "this file carries skins, blend shapes or animation stacks, which this build does "
                "not import: animation-and-skinning reaches Working at M8 and there is nothing to "
                "import a skeleton into before it. Meshes and materials came through.",
                request.source.view());
            !reported) {
            ufbx_free_scene(scene);
            return reported;
        }
    }

    SubAssetNames names;

    // --- 9. Materials.
    std::vector<std::string> material_names;
    for (usize index = 0; index < scene->materials.count; ++index) {
        const ufbx_material& source = *scene->materials.data[index];
        material_names.push_back(names.unique("material/", view_of(source.name), index));
        if (!state.import_materials) {
            continue;
        }
        Array<u8> payload;
        if (Status written = write_cooked_material(standard_material_of(source), payload);
            !written) {
            ufbx_free_scene(scene);
            return written;
        }
        if (Status added = out.add(assets::AssetKind::Material, material_names.back(),
                                   std::move(payload), false);
            !added) {
            ufbx_free_scene(scene);
            return added;
        }
    }

    // --- 2 to 5. Meshes. The built meshes are KEPT, because collision is generated from the source
    // mesh and the node that names a collider is not read until step 10.
    std::vector<MeshData> built_meshes;
    std::vector<std::string> mesh_names;
    for (usize index = 0; index < scene->meshes.count; ++index) {
        const ufbx_mesh& source = *scene->meshes.data[index];
        mesh_names.push_back(names.unique("mesh/", view_of(source.name), index));
        built_meshes.emplace_back();
        if (!state.import_meshes || source.num_faces == 0) {
            continue;
        }

        MeshData built;
        if (Status assembled = build_mesh(state, source, built); !assembled) {
            ufbx_free_scene(scene);
            return assembled;
        }
        if (built.indices.empty()) {
            continue;
        }
        if (Status finished = finish_mesh(built, state.build, out, mesh_names.back()); !finished) {
            ufbx_free_scene(scene);
            return finished;
        }
        if (Status emitted = emit_mesh_with_lods(built, mesh_names.back(), state.build, out);
            !emitted) {
            ufbx_free_scene(scene);
            return emitted;
        }
        built_meshes.back() = std::move(built);
    }

    // --- 10. The hierarchy, depth first from the root so a parent's index is always below its
    // children's.
    std::vector<ImportedNode> nodes;
    std::vector<std::string> node_names;
    std::vector<i32> node_source_mesh;
    std::vector<const ufbx_node*> stack_nodes;
    std::vector<i32> stack_parents;
    for (usize child = scene->root_node->children.count; child > 0; --child) {
        stack_nodes.push_back(scene->root_node->children.data[child - 1]);
        stack_parents.push_back(-1);
    }
    while (!stack_nodes.empty()) {
        const ufbx_node* source_node = stack_nodes.back();
        const i32 parent = stack_parents.back();
        stack_nodes.pop_back();
        stack_parents.pop_back();
        if (source_node == nullptr) {
            continue;
        }

        node_names.emplace_back(view_of(source_node->name));
        ImportedNode node;
        node.name = node_names.back();
        node.parent = parent;
        const ufbx_transform& local = source_node->local_transform;
        node.translation = to_vec3(local.translation) * state.scale;
        node.rotation =
            Quat{static_cast<f32>(local.rotation.x), static_cast<f32>(local.rotation.y),
                 static_cast<f32>(local.rotation.z), static_cast<f32>(local.rotation.w)};
        node.scale = to_vec3(local.scale);
        apply_root_override(state, node);

        i32 source_mesh = -1;
        if (source_node->mesh != nullptr && source_node->mesh->typed_id < mesh_names.size()) {
            node.mesh = static_cast<i32>(source_node->mesh->typed_id);
            source_mesh = node.mesh;
        }

        // "WHEN a node is named with the configured collision suffix THEN a collider SHALL be
        // generated from it and the node excluded from rendering."
        if (!state.build.collision_suffix.empty() &&
            ends_with(node.name, state.build.collision_suffix) &&
            state.build.collision_mode != "none" && node.mesh >= 0) {
            node.collision_only = true;
            node.mesh = -1;
        }

        const auto index_of_node = static_cast<i32>(nodes.size());
        nodes.push_back(node);
        node_source_mesh.push_back(source_mesh);

        for (usize child = source_node->children.count; child > 0; --child) {
            stack_nodes.push_back(source_node->children.data[child - 1]);
            stack_parents.push_back(index_of_node);
        }
    }
    // `node.name` points into `node_names`, which grew while the walk ran and may have reallocated.
    // Rebinding after the walk is what keeps those views valid — a bug that would otherwise appear
    // as a corrupted node name in a file with more than a handful of nodes.
    for (usize index = 0; index < nodes.size(); ++index) {
        nodes[index].name = node_names[index];
    }

    // --- 6. Collision, from the source mesh and never from a level of detail.
    usize colliders = 0;
    for (usize index = 0; index < nodes.size(); ++index) {
        ImportedNode& node = nodes[index];
        const i32 source_mesh = node_source_mesh[index];
        if (!node.collision_only || source_mesh < 0 ||
            static_cast<usize>(source_mesh) >= built_meshes.size()) {
            continue;
        }
        const MeshData& source = built_meshes[static_cast<usize>(source_mesh)];
        if (source.indices.empty()) {
            continue;
        }
        std::string collider_name = "collision/";
        collider_name += node.name;
        Expected<usize, Error> emitted = emit_collision(source, collider_name, state.build, out);
        if (!emitted) {
            ufbx_free_scene(scene);
            return make_unexpected(emitted.error());
        }
        if (emitted.value() != 0) {
            node.collision = static_cast<i32>(colliders);
            ++colliders;
        }
    }

    ufbx_free_scene(scene);
    return emit_prefab(Span<const ImportedNode>(nodes.data(), nodes.size()), out);
}

}  // namespace cy::import
