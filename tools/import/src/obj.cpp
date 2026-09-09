#include <cy/import/obj.h>

#include <cy/core/math/scalar.h>
#include <cy/import/model.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cy::import {
namespace {

// --- Options -------------------------------------------------------------------------------------
//
// The names are the glTF and FBX schemas', deliberately, and `test_obj.cpp` asserts it against both
// of them. See obj.h.

constexpr std::string_view kUpAxisChoices[] = {"y-up", "z-up"};
constexpr std::string_view kCollisionChoices[] = {"none", "convex", "decompose", "triangle"};

constexpr OptionSpec kObjOptions[] = {
    {"scale",
     OptionType::Float,
     OptionValue::of_float(1.0),
     "A uniform scale applied to every position at import, so that a model authored in centimetres "
     "becomes metres without any runtime code accounting for it. An OBJ declares no unit at all, "
     "so "
     "this is the only thing that can correct one.",
     {},
     1.0e-6,
     1.0e6},
    {"source-up-axis", OptionType::Enumeration, OptionValue::of_enumeration("y-up"),
     "Which axis is up in the source file. An OBJ declares no axis system, so there is no 'auto' "
     "to "
     "offer as FBX has: a z-up model is rotated at import, per 'no runtime code accounts for "
     "source "
     "handedness'.",
     Span<const std::string_view>(kUpAxisChoices), 0.0, 0.0},
    {"import-meshes",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to produce meshes. Turning it off is the fast path for iterating on materials alone.",
     {},
     0.0,
     0.0},
    {"import-materials",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to produce material sub-assets from the companion .mtl files this file names.",
     {},
     0.0,
     0.0},
    {"weld-tolerance",
     OptionType::Float,
     OptionValue::of_float(1.0e-5),
     "How close two positions must be, in metres, to be merged into one vertex. OBJ indexes "
     "position, texture coordinate and normal separately, so a face list has to be de-indexed to "
     "be "
     "read at all and this step is what turns it back into a vertex buffer.",
     {},
     0.0,
     1.0},
    {"smoothing-angle",
     OptionType::Float,
     OptionValue::of_float(60.0),
     "The angle in degrees beyond which two faces are a hard edge, used when the source supplies "
     "no "
     "normals. 180 makes everything smooth; 0 makes everything flat.",
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
     "should "
     "leave it off.",
     {},
     0.0,
     0.0},
    {"lightmap-texel-density",
     OptionType::Float,
     OptionValue::of_float(16.0),
     "Texels per world unit for the generated lightmap atlas. The atlas is sized from this and "
     "from "
     "the mesh's own surface area, so one number describes a whole project.",
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
     "An object whose name ends with this becomes a collider and is excluded from rendering. Empty "
     "disables the convention. OBJ has no nodes, so the name it is matched against is the `o` (or "
     "`g`) statement's, which is the only name the format carries.",
     {},
     0.0,
     0.0},
    {"collision-mode", OptionType::Enumeration, OptionValue::of_enumeration("convex"),
     "What a collision object produces: nothing, one convex hull, a bounded set of hulls from a "
     "convex decomposition, or the triangle mesh as it stands.",
     Span<const std::string_view>(kCollisionChoices), 0.0, 0.0},
};

constexpr std::string_view kExtensions[] = {".obj"};

/// What an OBJ can produce. No `Prefab`, and that is the format rather than an omission — see the
/// note on step 10 at the head of obj.h.
constexpr assets::AssetKind kProduces[] = {assets::AssetKind::Mesh, assets::AssetKind::Material};

// --- Text
// -----------------------------------------------------------------------------------------

[[nodiscard]] bool is_space(char character) noexcept {
    return character == ' ' || character == '\t' || character == '\r';
}

/// The line with its comment and its surrounding whitespace removed.
[[nodiscard]] std::string_view strip(std::string_view line) noexcept {
    const std::string_view::size_type hash = line.find('#');
    if (hash != std::string_view::npos) {
        line = line.substr(0, hash);
    }
    while (!line.empty() && is_space(line.front())) {
        line.remove_prefix(1);
    }
    while (!line.empty() && is_space(line.back())) {
        line.remove_suffix(1);
    }
    return line;
}

/// Take the next whitespace-delimited token off the front of `rest`.
[[nodiscard]] std::string_view take(std::string_view& rest) noexcept {
    while (!rest.empty() && is_space(rest.front())) {
        rest.remove_prefix(1);
    }
    usize length = 0;
    while (length < rest.size() && !is_space(rest[length])) {
        ++length;
    }
    const std::string_view token = rest.substr(0, length);
    rest.remove_prefix(length);
    return token;
}

/// Everything left of `rest`, trimmed. What a name-valued statement takes, because a file name may
/// contain spaces and `newmtl` and `mtllib` both allow it.
[[nodiscard]] std::string_view remainder(std::string_view rest) noexcept {
    while (!rest.empty() && is_space(rest.front())) {
        rest.remove_prefix(1);
    }
    while (!rest.empty() && is_space(rest.back())) {
        rest.remove_suffix(1);
    }
    return rest;
}

[[nodiscard]] f32 to_f32(std::string_view token, f32 fallback) noexcept {
    if (token.empty()) {
        return fallback;
    }
    // `std::strtof` wants a terminator and a token is a view into the caller's buffer, so the
    // digits are copied. A vertex line is under sixty characters; a token longer than the buffer is
    // not a number and takes the fallback.
    char digits[64] = {};
    if (token.size() >= sizeof(digits)) {
        return fallback;
    }
    for (usize index = 0; index < token.size(); ++index) {
        digits[index] = token[index];
    }
    char* end = nullptr;
    const float value = std::strtof(digits, &end);
    if (end == digits || !std::isfinite(value)) {
        return fallback;
    }
    return value;
}

/// One OBJ index: one-based, or negative and relative to the END of the array as it stands.
///
/// The relative form is the one dialect difference that silently corrupts a mesh when it is got
/// wrong, because a file using it still parses.
[[nodiscard]] bool to_index(std::string_view token, usize count, u32& out) noexcept {
    if (token.empty() || count == 0) {
        return false;
    }
    char digits[32] = {};
    if (token.size() >= sizeof(digits)) {
        return false;
    }
    for (usize index = 0; index < token.size(); ++index) {
        digits[index] = token[index];
    }
    char* end = nullptr;
    const long long value = std::strtoll(digits, &end, 10);
    if (end == digits || value == 0) {
        return false;
    }
    const long long resolved = value > 0 ? value - 1 : static_cast<long long>(count) + value;
    if (resolved < 0 || std::cmp_greater_equal(resolved, count)) {
        return false;
    }
    out = static_cast<u32>(resolved);
    return true;
}

// --- The OBJ parser
// ---------------------------------------------------------------------------------

/// One corner of one face, resolved against the file's own arrays.
struct Corner {
    u32 position = 0;
    u32 uv = 0;
    u32 normal = 0;
    bool has_uv = false;
    bool has_normal = false;
};

/// The file's shared arrays and where the parser has got to.
struct ObjParse {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uvs;
    ObjDocument document;
    /// Whether an `o` statement has been seen. Until one has, a `g` starts an object; after one
    /// has, a `g` is a subdivision of the object it is inside and is ignored. That is the rule
    /// every OBJ reader settles on, because exporters use `g` for both purposes and only `o` for
    /// one.
    bool saw_object = false;
};

[[nodiscard]] ObjObject& current_object(ObjParse& parse) {
    if (parse.document.objects.empty()) {
        parse.document.objects.emplace_back();
    }
    return parse.document.objects.back();
}

[[nodiscard]] ObjGroup& current_group(ObjParse& parse) {
    ObjObject& object = current_object(parse);
    if (object.groups.empty()) {
        object.groups.emplace_back();
    }
    return object.groups.back();
}

void begin_object(ObjParse& parse, std::string_view name) {
    ObjObject object;
    object.name = std::string(name);
    parse.document.objects.push_back(std::move(object));
}

void begin_group(ObjParse& parse, std::string_view material) {
    ObjObject& object = current_object(parse);
    // An empty leading group — `usemtl` before any face — is reused rather than left behind, so a
    // well-formed file produces exactly one section per material run.
    if (!object.groups.empty() && object.groups.back().indices.empty()) {
        object.groups.back().name = std::string(material);
        return;
    }
    ObjGroup group;
    group.name = std::string(material);
    object.groups.push_back(std::move(group));
}

/// Resolve one `v/vt/vn` token. Returns false for a corner naming an index the file does not have.
[[nodiscard]] bool parse_corner(const ObjParse& parse, std::string_view token,
                                Corner& out) noexcept {
    std::string_view::size_type first = token.find('/');
    const std::string_view position = token.substr(0, first);
    if (!to_index(position, parse.positions.size(), out.position)) {
        return false;
    }
    if (first == std::string_view::npos) {
        return true;
    }
    std::string_view rest = token.substr(first + 1);
    const std::string_view::size_type second = rest.find('/');
    const std::string_view uv = rest.substr(0, second);
    out.has_uv = !uv.empty() && to_index(uv, parse.uvs.size(), out.uv);
    if (second == std::string_view::npos) {
        return true;
    }
    const std::string_view normal = rest.substr(second + 1);
    out.has_normal = !normal.empty() && to_index(normal, parse.normals.size(), out.normal);
    return true;
}

/// Append one corner to the current object, de-indexed, and index it in the current group.
void append_corner(ObjParse& parse, const Corner& corner) {
    ObjObject& object = current_object(parse);
    const auto vertex = static_cast<u32>(object.positions.size());
    object.positions.push_back(parse.positions[corner.position]);
    if (corner.has_normal) {
        object.normals.push_back(parse.normals[corner.normal]);
    }
    if (corner.has_uv) {
        object.uvs.push_back(parse.uvs[corner.uv]);
    }
    current_group(parse).indices.push_back(vertex);
}

/// One `f` statement, triangulated by a fan. See obj.h for why a fan.
void parse_face(ObjParse& parse, std::string_view rest) {
    std::vector<Corner> corners;
    while (true) {
        const std::string_view token = take(rest);
        if (token.empty()) {
            break;
        }
        Corner corner;
        if (!parse_corner(parse, token, corner)) {
            ++parse.document.malformed_faces;
            return;
        }
        corners.push_back(corner);
    }
    if (corners.size() < 3) {
        ++parse.document.malformed_faces;
        return;
    }
    for (usize triangle = 1; triangle + 1 < corners.size(); ++triangle) {
        append_corner(parse, corners[0]);
        append_corner(parse, corners[triangle]);
        append_corner(parse, corners[triangle + 1]);
    }
}

/// Handle one statement. Split out of `parse_obj` so that neither the loop nor this is a function
/// with a switch over twelve keywords inside a loop over lines.
void parse_statement(ObjParse& parse, std::string_view keyword, std::string_view rest) {
    if (keyword == "v") {
        const f32 x = to_f32(take(rest), 0.0f);
        const f32 y = to_f32(take(rest), 0.0f);
        const f32 z = to_f32(take(rest), 0.0f);
        parse.positions.push_back(Vec3{x, y, z});
    } else if (keyword == "vn") {
        const f32 x = to_f32(take(rest), 0.0f);
        const f32 y = to_f32(take(rest), 0.0f);
        const f32 z = to_f32(take(rest), 0.0f);
        parse.normals.push_back(Vec3{x, y, z});
    } else if (keyword == "vt") {
        const f32 u = to_f32(take(rest), 0.0f);
        const f32 v = to_f32(take(rest), 0.0f);
        parse.uvs.push_back(Vec2{u, v});
    } else if (keyword == "f") {
        parse_face(parse, rest);
    } else if (keyword == "o") {
        parse.saw_object = true;
        begin_object(parse, remainder(rest));
    } else if (keyword == "g") {
        if (!parse.saw_object) {
            begin_object(parse, remainder(rest));
        }
    } else if (keyword == "usemtl") {
        begin_group(parse, remainder(rest));
    } else if (keyword == "mtllib") {
        // One statement may name several libraries, and a file may name one twice.
        while (true) {
            const std::string_view library = take(rest);
            if (library.empty()) {
                break;
            }
            bool seen = false;
            for (const std::string& existing : parse.document.material_libraries) {
                seen = seen || existing == library;
            }
            if (!seen) {
                parse.document.material_libraries.emplace_back(library);
            }
        }
    } else if (keyword != "s" && keyword != "l" && keyword != "p" && keyword != "mtllib" &&
               keyword != "vp") {
        // `s`, `l`, `p` and `vp` are understood and deliberately unused: smoothing groups are
        // superseded by the smoothing angle the engine's own normal generation takes, and lines,
        // points and parameter-space vertices are not renderable geometry in this engine.
        ++parse.document.unsupported_lines;
    }
}

/// Drop an attribute array the file supplied for only some corners.
///
/// Partial normals or partial texture coordinates cannot be used: the arrays are parallel, so a
/// missing entry shifts every later one. Dropping the array is what makes the mesh correct —
/// `finish_mesh` then generates the normals, and a mesh with no UVs simply has none.
void settle_attributes(ObjObject& object) {
    if (object.normals.size() != object.positions.size()) {
        object.normals.clear();
    }
    if (object.uvs.size() != object.positions.size()) {
        object.uvs.clear();
    }
    std::erase_if(object.groups, [](const ObjGroup& group) { return group.indices.empty(); });
}

// --- The MTL parser
// ---------------------------------------------------------------------------------

/// Blinn-Phong specular exponent to a roughness, which is the conversion every OBJ importer makes.
///
/// `roughness = sqrt(2 / (Ns + 2))`. It is the inverse of the exponent a GGX lobe of that roughness
/// approximates, and it is right at both ends: `Ns = 0` gives 1 and a large exponent tends to 0.
[[nodiscard]] f32 roughness_of_exponent(f32 exponent) noexcept {
    const f32 clamped = exponent < 0.0f ? 0.0f : exponent;
    return std::sqrt(2.0f / (clamped + 2.0f));
}

/// `explicit_roughness` is set by `Pr` and consulted by `Ns`, so a file carrying the PBR extension
/// keeps it whichever order the two statements appear in — which matters because an `.mtl` written
/// by a modern exporter states both and the classic one is the approximation.
void parse_mtl_statement(ObjMaterial& material, std::string_view keyword, std::string_view rest,
                         bool& explicit_roughness) {
    if (keyword == "Kd") {
        material.standard.base_colour[0] = to_f32(take(rest), 1.0f);
        material.standard.base_colour[1] = to_f32(take(rest), 1.0f);
        material.standard.base_colour[2] = to_f32(take(rest), 1.0f);
    } else if (keyword == "Ke") {
        material.standard.emissive[0] = to_f32(take(rest), 0.0f);
        material.standard.emissive[1] = to_f32(take(rest), 0.0f);
        material.standard.emissive[2] = to_f32(take(rest), 0.0f);
    } else if (keyword == "d") {
        material.standard.base_colour[3] = to_f32(take(rest), 1.0f);
    } else if (keyword == "Tr") {
        // `Tr` is transparency and `d` is dissolve — the same quantity, inverted. A file that
        // carries both is contradicting itself and the last statement wins, which is what a
        // line-based format means by precedence.
        material.standard.base_colour[3] = 1.0f - to_f32(take(rest), 0.0f);
    } else if (keyword == "Ns") {
        const f32 roughness = roughness_of_exponent(to_f32(take(rest), 0.0f));
        if (!explicit_roughness) {
            material.standard.roughness = roughness;
        }
    } else if (keyword == "Pr") {
        material.standard.roughness = to_f32(take(rest), 1.0f);
        explicit_roughness = true;
    } else if (keyword == "Pm") {
        material.standard.metallic = to_f32(take(rest), 0.0f);
    } else if (keyword.starts_with("map_") || keyword == "bump" || keyword == "norm" ||
               keyword == "refl") {
        // The last token of a map statement is the file; everything before it is options such as
        // `-bm 0.5` or `-s 1 1 1`, which this importer does not act on and does not need to parse
        // to find the name.
        std::string_view file;
        while (true) {
            const std::string_view token = take(rest);
            if (token.empty()) {
                break;
            }
            file = token;
        }
        if (!file.empty()) {
            material.textures.emplace_back(file);
        }
    }
}

// --- Building a mesh out of one object
// --------------------------------------------------------------

/// Everything read out of the option schema, so `import` reads as the ordered list of steps.
struct ObjState {
    OptionsSchema options;
    f32 scale = 1.0f;
    bool z_up = false;
    bool import_meshes = true;
    bool import_materials = true;
    ModelBuildOptions build;
};

/// Read every declared option into `state`, so that `import` reads as the ordered list of steps.
///
/// One function rather than seventeen inline lookups inside the import, and one `if` rather than
/// seventeen: an option this importer declares and cannot read back is a defect in its own schema
/// and not in the file, so it fails as `Internal` and names itself.
[[nodiscard]] Status read_options(const ImportRequest& request, ObjState& state) noexcept {
    const auto option = [&](std::string_view name) noexcept {
        return request.option(state.options, name);
    };
    Expected<OptionValue, Error> scale = option("scale");
    Expected<OptionValue, Error> up = option("source-up-axis");
    Expected<OptionValue, Error> meshes = option("import-meshes");
    Expected<OptionValue, Error> materials_wanted = option("import-materials");
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
    if (!scale || !up || !meshes || !materials_wanted || !weld_tolerance || !smoothing ||
        !tangents || !optimise_option || !overdraw || !lightmap || !density || !padding ||
        !lod_count || !lod_ratio || !lod_error || !collision_suffix || !collision_mode) {
        return fail(ErrorCode::Internal, "the OBJ importer's own option schema is inconsistent");
    }
    state.scale = static_cast<f32>(scale.value().as_float());
    state.z_up = up.value().as_text() == "z-up";
    state.import_meshes = meshes.value().as_bool();
    state.import_materials = materials_wanted.value().as_bool();
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

    return ok();
}

/// Step 1's whole content: the change of frame, applied to a direction.
[[nodiscard]] Vec3 oriented(const ObjState& state, const Vec3& value) noexcept {
    return state.z_up ? Vec3{value.x, value.z, -value.y} : value;
}

/// Step 2: one object as `MeshData`, split into sections by its `usemtl` runs.
[[nodiscard]] Status build_mesh(const ObjState& state, const ObjObject& object,
                                const std::vector<std::string>& material_names,
                                MeshData& out) noexcept {
    for (const Vec3& position : object.positions) {
        if (Status pushed = out.positions.push_back(oriented(state, position) * state.scale);
            !pushed) {
            return pushed;
        }
    }
    for (const Vec3& normal : object.normals) {
        if (Status pushed = out.normals.push_back(oriented(state, normal)); !pushed) {
            return pushed;
        }
    }
    for (const Vec2& uv : object.uvs) {
        if (Status pushed = out.uvs.push_back(uv); !pushed) {
            return pushed;
        }
    }

    for (const ObjGroup& group : object.groups) {
        const auto first_index = static_cast<u32>(out.indices.size());
        for (const u32 index : group.indices) {
            if (Status pushed = out.indices.push_back(index); !pushed) {
                return pushed;
            }
        }
        MeshSection section;
        section.first_index = first_index;
        section.index_count = static_cast<u32>(group.indices.size());
        section.material = 0;
        for (usize slot = 0; slot < material_names.size(); ++slot) {
            if (material_names[slot] == group.name) {
                section.material = static_cast<u32>(slot);
                break;
            }
        }
        if (section.index_count == 0) {
            continue;
        }
        if (Status pushed = out.sections.push_back(section); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// Whether an object's name puts it under the collision naming convention.
///
/// The name is the `o` statement's, because OBJ has no nodes and that is the only name the format
/// carries. `ends_with` is `model.h`'s, so the convention is the same predicate the glTF and FBX
/// importers apply to a node name.
[[nodiscard]] bool is_collider(const ObjState& state, const ObjObject& object) noexcept {
    return !state.build.collision_suffix.empty() && state.build.collision_mode != "none" &&
           ends_with(object.name, state.build.collision_suffix);
}

/// Read every texture the materials name, so that each is recorded as a dependency.
///
/// The textures themselves are imported by the TEXTURE importer. A model importer that cooked them
/// would produce a second cooked texture and a second id for one image, which is the defect
/// `fbx.cpp` states at the same point in its own import.
[[nodiscard]] Status record_textures(const ImportRequest& request,
                                     const std::vector<ObjMaterial>& materials,
                                     ImportResult& out) noexcept {
    if (request.resolver == nullptr) {
        return ok();
    }
    for (const ObjMaterial& material : materials) {
        for (const std::string& texture : material.textures) {
            if (Expected<Span<const u8>, Error> bytes = request.resolver->read(texture); bytes) {
                continue;
            }
            if (Status reported =
                    out.report(ImportSeverity::Warning, "missing-texture",
                               "a texture this model references is not beside it; the material "
                               "will resolve to a placeholder until it is added",
                               texture);
                !reported) {
                return reported;
            }
        }
    }
    return ok();
}

/// Read every `.mtl` this file names, through the resolver, and parse them into one list.
///
/// Reading through `ImportResolver::read` is what records each library as a dependency, so editing
/// a colour in an `.mtl` invalidates the model that uses it. A library that is not there is a
/// WARNING about the file — a reference to something absent is a real defect in the delivery, which
/// is a different thing from a step the format cannot express.
[[nodiscard]] Status read_materials(const ImportRequest& request, const ObjDocument& document,
                                    std::vector<ObjMaterial>& out_materials,
                                    ImportResult& out) noexcept {
    for (const std::string& library : document.material_libraries) {
        if (request.resolver == nullptr) {
            continue;
        }
        Expected<Span<const u8>, Error> bytes = request.resolver->read(library);
        if (!bytes) {
            if (Status reported =
                    out.report(ImportSeverity::Warning, "missing-mtl",
                               "this model names a material library that is not beside it; its "
                               "materials will resolve to the standard defaults until it is added",
                               library);
                !reported) {
                return reported;
            }
            continue;
        }
        const std::string_view text(reinterpret_cast<const char*>(bytes.value().data()),
                                    bytes.value().size());
        Expected<std::vector<ObjMaterial>, Error> parsed = parse_mtl(text);
        if (!parsed) {
            return make_unexpected(parsed.error());
        }
        for (ObjMaterial& material : parsed.value()) {
            bool duplicate = false;
            for (const ObjMaterial& existing : out_materials) {
                duplicate = duplicate || existing.name == material.name;
            }
            if (duplicate) {
                // Two libraries defining one name is a project mistake and the FIRST wins, because
                // that is the order the file states them in and a silent second opinion about a
                // material is worse than a stated one.
                if (Status reported = out.report(ImportSeverity::Warning, "duplicate-material",
                                                 "two material libraries define this name; the "
                                                 "first library's definition was used",
                                                 material.name);
                    !reported) {
                    return reported;
                }
                continue;
            }
            out_materials.push_back(std::move(material));
        }
    }

    return record_textures(request, out_materials, out);
}

/// The file's own name, without its directory or its extension. What an unnamed object is called.
[[nodiscard]] std::string_view stem_of(std::string_view path) noexcept {
    const std::string_view::size_type slash = path.find_last_of('/');
    if (slash != std::string_view::npos) {
        path = path.substr(slash + 1);
    }
    const std::string_view::size_type dot = path.find_last_of('.');
    if (dot != std::string_view::npos && dot != 0) {
        path = path.substr(0, dot);
    }
    return path;
}

/// Steps 2 to 6, for every object in the file, and how many sub-assets they produced.
///
/// Split out of `import` so that the import itself reads as the ordered list of steps the
/// specification fixes, which is what the comments in it claim and what a reader checks them
/// against.
[[nodiscard]] Expected<usize, Error> emit_objects(const ObjState& state,
                                                  const ObjDocument& document,
                                                  const std::vector<std::string>& material_names,
                                                  const ImportRequest& request,
                                                  SubAssetNames& names,
                                                  ImportResult& out) noexcept {
    // --- 2 to 6. The meshes.
    //
    // The PRIMARY sub-asset is decided before anything is emitted, because `ImportResult::add`
    // takes the flag and a sub-asset cannot be promoted afterwards. An `.obj` IS a mesh — there is
    // no prefab for a reference to the file to resolve to, which is step 10 being absent — so the
    // primary is its first render mesh. A file holding nothing but collision proxies has no render
    // mesh to nominate and gets none; the pipeline's own fallback then takes the first collider,
    // which is still a mesh and still the right kind for the `.meta`.
    usize primary_object = document.objects.size();
    for (usize index = 0; index < document.objects.size(); ++index) {
        if (!is_collider(state, document.objects[index])) {
            primary_object = index;
            break;
        }
    }

    usize emitted = 0;
    for (usize index = 0; index < document.objects.size(); ++index) {
        const ObjObject& object = document.objects[index];
        const std::string_view raw_name =
            object.name.empty() ? stem_of(request.source.view()) : std::string_view(object.name);
        const bool collider = is_collider(state, object);
        const std::string name = names.unique(collider ? "collision/" : "mesh/", raw_name, index);
        if (!state.import_meshes) {
            continue;
        }

        MeshData mesh;
        if (Status assembled = build_mesh(state, object, material_names, mesh); !assembled) {
            return make_unexpected(assembled.error());
        }
        if (mesh.indices.empty()) {
            continue;
        }
        // Steps 3 and 4: the normals the file did not supply, the weld that undoes OBJ's
        // per-corner indexing, the tangent basis and the three optimisers — all of it `model.h`'s
        // and all of it the same code the other two importers run.
        if (Status finished = finish_mesh(mesh, state.build, out, name); !finished) {
            return make_unexpected(finished.error());
        }
        if (collider) {
            // Step 6. Generated from the mesh as it was built and never from a level of detail,
            // which is why the collision branch takes `mesh` rather than anything reduced.
            Expected<usize, Error> produced = emit_collision(mesh, name, state.build, out);
            if (!produced) {
                return make_unexpected(produced.error());
            }
            emitted += produced.value();
            continue;
        }
        // Step 5 rides along with the emission: `emit_mesh_with_lods` writes the full-detail mesh
        // and then the chain, each level simplified from the one above it.
        if (Status added =
                emit_mesh_with_lods(mesh, name, state.build, out, index == primary_object);
            !added) {
            return make_unexpected(added.error());
        }
        ++emitted;
    }

    return emitted;
}

}  // namespace

// --- The public parsers
// -----------------------------------------------------------------------------

Expected<ObjDocument, Error> parse_obj(std::string_view text) noexcept {
    ObjParse parse;
    while (!text.empty()) {
        const std::string_view::size_type newline = text.find('\n');
        const std::string_view raw =
            newline == std::string_view::npos ? text : text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);

        std::string_view line = strip(raw);
        if (line.empty()) {
            continue;
        }
        const std::string_view keyword = take(line);
        parse_statement(parse, keyword, line);
    }
    for (ObjObject& object : parse.document.objects) {
        settle_attributes(object);
    }
    std::erase_if(parse.document.objects,
                  [](const ObjObject& object) { return object.groups.empty(); });
    return std::move(parse.document);
}

Expected<std::vector<ObjMaterial>, Error> parse_mtl(std::string_view text) noexcept {
    std::vector<ObjMaterial> materials;
    bool explicit_roughness = false;
    while (!text.empty()) {
        const std::string_view::size_type newline = text.find('\n');
        const std::string_view raw =
            newline == std::string_view::npos ? text : text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);

        std::string_view line = strip(raw);
        if (line.empty()) {
            continue;
        }
        const std::string_view keyword = take(line);
        if (keyword == "newmtl") {
            ObjMaterial material;
            material.name = std::string(remainder(line));
            // An `.mtl` describes a specular-glossiness model and the engine's standard material is
            // metallic-roughness. Nothing in the format says how metallic a surface is, so a
            // material with no `Pm` is dielectric — which is right for all but a handful of
            // materials, and the alternative default (the standard material's own 1.0) would make
            // every OBJ in the world import as polished metal.
            material.standard.metallic = 0.0f;
            materials.push_back(std::move(material));
            explicit_roughness = false;
            continue;
        }
        if (materials.empty()) {
            continue;
        }
        parse_mtl_statement(materials.back(), keyword, line, explicit_roughness);
    }
    for (ObjMaterial& material : materials) {
        material.standard.alpha_mode = material.standard.base_colour[3] < 1.0f ? 2U : 0U;
    }
    return materials;
}

// --- The importer
// -------------------------------------------------------------------------------------

OptionsSchema obj_options() noexcept {
    return OptionsSchema(Span<const OptionSpec>(kObjOptions));
}

ImporterInfo ObjImporter::info() const noexcept {
    ImporterInfo info;
    info.name = "obj";
    info.version = 1;
    info.extensions = Span<const std::string_view>(kExtensions);
    info.produces = Span<const assets::AssetKind>(kProduces);
    info.description =
        "Imports a Wavefront OBJ and its companion .mtl into cooked meshes with levels of detail, "
        "standard materials and collision proxies from a naming convention. The format carries no "
        "rig, no animation and no scene graph, so steps 7, 8 and 10 of the model import sequence "
        "are not reached and the report says so. Every step after the parse is shared with the "
        "glTF and FBX importers.";
    info.steps = kFlatModelSteps;
    return info;
}

OptionsSchema ObjImporter::schema() const noexcept {
    return obj_options();
}

Status ObjImporter::import(const ImportRequest& request, ImportResult& out) noexcept {
    ObjState state;
    state.options = schema();
    if (Status read = read_options(request, state); !read) {
        return read;
    }

    // --- 1. Parse.
    const std::string_view text(reinterpret_cast<const char*>(request.bytes.data()),
                                request.bytes.size());
    Expected<ObjDocument, Error> parsed = parse_obj(text);
    if (!parsed) {
        return make_unexpected(parsed.error());
    }
    const ObjDocument& document = parsed.value();
    if (document.objects.empty()) {
        return out.report(ImportSeverity::Error, "empty-obj",
                          "this file declares no triangles: it has no `f` statement whose corners "
                          "name vertices the file supplies",
                          request.source.view());
    }
    if (document.malformed_faces != 0) {
        char detail[ImportDiagnostic::kDetailCapacity] = {};
        (void)std::snprintf(
            detail, sizeof(detail),
            "%zu face(s) were dropped: fewer than three corners, or a corner naming "
            "a vertex the file does not declare",
            document.malformed_faces);
        if (Status reported = out.report(ImportSeverity::Warning, "malformed-face", detail,
                                         request.source.view());
            !reported) {
            return reported;
        }
    }

    // --- 9. Materials, out of the companion `.mtl`.
    //
    // Parsed BEFORE the meshes because a section's material index is an index into this list, and
    // EMITTED after them, which is the specification's own order (step 9 follows step 6) and which
    // also decides what a reference to the file resolves to: the pipeline falls back to the first
    // sub-asset when there is no primary, and the first sub-asset should be a mesh.
    std::vector<ObjMaterial> materials;
    if (Status collected = read_materials(request, document, materials, out); !collected) {
        return collected;
    }
    std::vector<std::string> material_names;
    material_names.reserve(materials.size());
    for (const ObjMaterial& material : materials) {
        material_names.push_back(material.name);
    }
    // Shared with the materials emitted at the end, so a material and a mesh that spell one name
    // still take two sub-asset names — and therefore two ids that never move.
    SubAssetNames names;

    // --- 2 to 6. The meshes.
    Expected<usize, Error> emitted =
        emit_objects(state, document, material_names, request, names, out);
    if (!emitted) {
        return make_unexpected(emitted.error());
    }
    if (emitted.value() == 0 && state.import_meshes) {
        return out.report(ImportSeverity::Error, "empty-obj",
                          "every object in this file produced no triangles after parsing",
                          request.source.view());
    }

    // Step 9's emission. `SubAssetNames` is shared with the meshes above, so a material and a mesh
    // that share a name still take two sub-asset names — and therefore two ids that never move.
    if (!state.import_materials) {
        return ok();
    }
    for (usize index = 0; index < materials.size(); ++index) {
        Array<u8> payload;
        if (Status written = write_cooked_material(materials[index].standard, payload); !written) {
            return written;
        }
        if (Status added = out.add(assets::AssetKind::Material,
                                   names.unique("material/", materials[index].name, index),
                                   std::move(payload), false);
            !added) {
            return added;
        }
    }
    return ok();
}

}  // namespace cy::import
