#include <cy/import/gltf.h>

#include <cy/core/assets/hash.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/scalar.h>
#include <cy/core/memory/scope.h>
#include <cy/import/clip_record.h>
#include <cy/import/fbx_skeleton.h>
#include <cy/import/json.h>

#ifdef CY_IMPORT_ANIMATION
// The definition, which `cy/import/clip_record.h` only forward-declares — see the note there for
// why a public header may not include it.
#    include <cy/animation/clip.h>
#endif
#include <cy/import/model.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <string>
#include <utility>
#include <vector>

namespace cy::import {
namespace {

// --- The byte writer and reader both payloads share ----------------------------------------------

void put_u32(Array<u8>& out, u32 value) noexcept {
    for (u32 index = 0; index < 4; ++index) {
        (void)out.push_back(static_cast<u8>((value >> (index * 8U)) & 0xFFU));
    }
}

void put_u16(Array<u8>& out, u16 value) noexcept {
    (void)out.push_back(static_cast<u8>(value & 0xFFU));
    (void)out.push_back(static_cast<u8>((value >> 8U) & 0xFFU));
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

[[nodiscard]] u16 get_u16(const u8* data) noexcept {
    return static_cast<u16>(static_cast<u16>(data[0]) | static_cast<u16>(data[1] << 8U));
}

[[nodiscard]] f32 get_f32(const u8* data) noexcept {
    const u32 bits = get_u32(data);
    f32 value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

// --- Options -------------------------------------------------------------------------------------

constexpr std::string_view kUpAxisChoices[] = {"y-up", "z-up"};
constexpr std::string_view kCollisionChoices[] = {"none", "convex", "decompose", "triangle"};

constexpr OptionSpec kGltfOptions[] = {
    {"scale",
     OptionType::Float,
     OptionValue::of_float(1.0),
     "A uniform scale applied to every position and translation at import, so that a model "
     "authored "
     "in centimetres becomes metres without any runtime code accounting for it.",
     {},
     1.0e-6,
     1.0e6},
    {"source-up-axis", OptionType::Enumeration, OptionValue::of_enumeration("y-up"),
     "Which axis is up in the source file. A z-up model is rotated at import, per 'no runtime code "
     "accounts for source handedness'.",
     Span<const std::string_view>(kUpAxisChoices), 0.0, 0.0},
    {"import-meshes",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to produce meshes. Turning it off with animations on is the fast path for animation "
     "iteration.",
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
     "How close two positions must be, in metres, to be merged into one vertex. Normals and "
     "texture "
     "coordinates still have to agree, so a hard edge and a texture seam survive welding.",
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
     "should leave it off. A source that already supplies TEXCOORD_1 keeps it.",
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
     "disables the convention.",
     {},
     0.0,
     0.0},
    {"collision-mode", OptionType::Enumeration, OptionValue::of_enumeration("convex"),
     "What a collision node produces: nothing, a convex hull, or the triangle mesh as it stands.",
     Span<const std::string_view>(kCollisionChoices), 0.0, 0.0},
    // --- Steps 7 and 8. M11.b task 6.1.
    //
    // EVERY ONE OF THEM CHANGES THE COOKED BYTES, which is why each is a declared option rather
    // than a constant somebody tunes later: `options.h` calls a setting that changes the output and
    // cannot reach the derivation key the one defect a cook cache cannot survive. They are spelled
    // exactly as `kFbxAnimationOptions` spells them, so a project that re-exports a character in
    // the other format keeps its settings — which is the reason `model.h` exists.
    {"import-skins",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to produce a skeleton and to carry each vertex's joint bindings into the cooked "
     "mesh. Off imports the same file as a static model, which is what a background prop exported "
     "from a rigged scene wants.",
     {},
     0.0,
     0.0},
    {"import-animations",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to produce animation clips from the file's animations. Turning meshes and materials "
     "off and leaving this on is the fast path for animation iteration.",
     {},
     0.0,
     0.0},
    {"animation-sample-rate",
     OptionType::Float,
     OptionValue::of_float(30.0),
     "The clip's own sample rate hint. glTF stores keys at the times the exporter wrote them and "
     "this importer resamples nothing, so unlike the FBX path this figure does not move a key — it "
     "is what a consumer is told the clip was authored at.",
     {},
     1.0,
     240.0},
    {"animation-translation-tolerance-mm",
     OptionType::Float,
     OptionValue::of_float(0.1),
     "How far, in millimetres, the curve fit may deviate from the authored translation. The codec "
     "then quantises what the fit kept and adds its own error on top, so the figure the import "
     "report names is MEASURED against the authored keys and can exceed this one.",
     {},
     0.0001,
     100.0},
    {"animation-rotation-tolerance-degrees",
     OptionType::Float,
     OptionValue::of_float(0.1),
     "How far, in degrees, the curve fit may deviate from the authored rotation. Quantising a "
     "rotation to four 16-bit components costs about a twentieth of a degree by itself, so the "
     "measured worst case sits a little above this number.",
     {},
     0.0001,
     45.0},
};

constexpr std::string_view kExtensions[] = {".gltf", ".glb"};
// `Animation` carries the skeleton and the clips both, which is the kind M8.d chose for the FBX
// path and the reason `fbx_skeleton.h` gives for it: a sub-asset NAME prefix tells a skeleton from
// a clip, and an `AssetKind` is persistent in a way a prefix is not.
constexpr assets::AssetKind kProduces[] = {assets::AssetKind::Mesh, assets::AssetKind::Material,
                                           assets::AssetKind::Prefab, assets::AssetKind::Animation};

// --- glTF component types ------------------------------------------------------------------------

constexpr i64 kComponentByte = 5120;
constexpr i64 kComponentUnsignedByte = 5121;
constexpr i64 kComponentShort = 5122;
constexpr i64 kComponentUnsignedShort = 5123;
constexpr i64 kComponentUnsignedInt = 5125;
constexpr i64 kComponentFloat = 5126;

[[nodiscard]] u32 component_size(i64 component_type) noexcept {
    switch (component_type) {
        case kComponentByte:
        case kComponentUnsignedByte:
            return 1;
        case kComponentShort:
        case kComponentUnsignedShort:
            return 2;
        case kComponentUnsignedInt:
        case kComponentFloat:
            return 4;
        default:
            return 0;
    }
}

[[nodiscard]] u32 component_count(std::string_view type) noexcept {
    if (type == "SCALAR") {
        return 1;
    }
    if (type == "VEC2") {
        return 2;
    }
    if (type == "VEC3") {
        return 3;
    }
    if (type == "VEC4") {
        return 4;
    }
    if (type == "MAT4") {
        return 16;
    }
    return 0;
}

/// Base64, for a glTF that embeds its buffer in a data URI. Rejects a wrong length or a stray
/// character rather than decoding as much as it can: a truncated buffer produces a mesh with a
/// plausible shape and missing triangles, which is far harder to diagnose than a refusal.
[[nodiscard]] Expected<std::vector<u8>, Error> decode_base64(std::string_view text) noexcept {
    const auto value_of = [](char character) noexcept -> i32 {
        if (character >= 'A' && character <= 'Z') {
            return character - 'A';
        }
        if (character >= 'a' && character <= 'z') {
            return (character - 'a') + 26;
        }
        if (character >= '0' && character <= '9') {
            return (character - '0') + 52;
        }
        if (character == '+') {
            return 62;
        }
        if (character == '/') {
            return 63;
        }
        return -1;
    };

    std::vector<u8> bytes;
    if (text.size() % 4 != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a base64 payload whose length is not a multiple of four");
    }
    bytes.reserve((text.size() / 4) * 3);
    for (usize index = 0; index < text.size(); index += 4) {
        i32 quad[4] = {0, 0, 0, 0};
        u32 padding = 0;
        for (u32 slot = 0; slot < 4; ++slot) {
            const char character = text[index + slot];
            if (character == '=') {
                quad[slot] = 0;
                ++padding;
                continue;
            }
            quad[slot] = value_of(character);
            if (quad[slot] < 0) {
                return fail(ErrorCode::InvalidArgument, "a character base64 does not use");
            }
        }
        const u32 packed = (static_cast<u32>(quad[0]) << 18U) | (static_cast<u32>(quad[1]) << 12U) |
                           (static_cast<u32>(quad[2]) << 6U) | static_cast<u32>(quad[3]);
        bytes.push_back(static_cast<u8>((packed >> 16U) & 0xFFU));
        if (padding < 2) {
            bytes.push_back(static_cast<u8>((packed >> 8U) & 0xFFU));
        }
        if (padding < 1) {
            bytes.push_back(static_cast<u8>(packed & 0xFFU));
        }
    }
    return bytes;
}

/// The whole of one glTF document, resolved: the JSON, the buffers' bytes, and the parsed options.
struct Document {
    JsonDocument json;
    /// One entry per glTF buffer. Either a view into the GLB's own BIN chunk or an owned decode.
    std::vector<std::vector<u8>> owned_buffers;
    std::vector<Span<const u8>> buffers;
};

/// Split a `.glb` container into its JSON and BIN chunks. `.gltf` is the whole input as JSON.
[[nodiscard]] Status split_container(Span<const u8> bytes, std::string_view& out_json,
                                     Span<const u8>& out_binary) noexcept {
    constexpr u8 kMagic[4] = {'g', 'l', 'T', 'F'};
    if (bytes.size() < 4 || std::memcmp(bytes.data(), kMagic, 4) != 0) {
        out_json = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        out_binary = {};
        return ok();
    }
    if (bytes.size() < 12) {
        return fail(ErrorCode::InvalidArgument, "a GLB shorter than its own header");
    }
    const u32 version = get_u32(bytes.data() + 4);
    if (version != 2) {
        return fail(ErrorCode::Unsupported, "a GLB container version other than 2");
    }
    usize cursor = 12;
    bool have_json = false;
    while (cursor + 8 <= bytes.size()) {
        const u32 length = get_u32(bytes.data() + cursor);
        const u32 type = get_u32(bytes.data() + cursor + 4);
        cursor += 8;
        if (cursor + length > bytes.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "a GLB chunk that runs past the end of the file");
        }
        if (type == 0x4E4F534AU) {  // 'JSON'
            out_json =
                std::string_view(reinterpret_cast<const char*>(bytes.data() + cursor), length);
            have_json = true;
        } else if (type == 0x004E4942U) {  // 'BIN\0'
            out_binary = Span<const u8>(bytes.data() + cursor, length);
        }
        // Chunks are four-byte aligned, and an unknown chunk type is skipped rather than refused —
        // the container format says a reader must, and future extensions rely on it.
        cursor += (length + 3U) & ~usize{3U};
    }
    if (!have_json) {
        return fail(ErrorCode::InvalidArgument, "a GLB with no JSON chunk");
    }
    return ok();
}

/// Read an accessor into floats, `component_count(type)` per element.
[[nodiscard]] Expected<std::vector<f32>, Error> read_accessor_floats(const Document& document,
                                                                     JsonRef accessor) noexcept {
    const JsonDocument& json = document.json;
    const i64 component_type = json.integer_or(json.member(accessor, "componentType"), 0);
    const auto count = static_cast<usize>(json.integer_or(json.member(accessor, "count"), 0));
    const std::string_view type = json.string_or(json.member(accessor, "type"), "");
    const bool normalized = json.bool_or(json.member(accessor, "normalized"), false);
    const u32 lanes = component_count(type);
    const u32 element = component_size(component_type);
    if (lanes == 0 || element == 0 || count == 0) {
        return fail(ErrorCode::InvalidArgument, "an accessor with an unusable type or count");
    }
    if (json.member(accessor, "sparse") != JsonDocument::kNone) {
        return fail(ErrorCode::Unsupported,
                    "a sparse accessor; re-export without sparse storage, which exporters use for "
                    "morph targets this build does not import");
    }

    const JsonRef view_ref = json.member(accessor, "bufferView");
    if (view_ref == JsonDocument::kNone) {
        // A bufferView-less accessor is defined to be all zeroes. Producing zeroes is correct and
        // is almost always a mistake in the source, so it is worth being explicit about rather than
        // silently supplying a mesh at the origin.
        return fail(ErrorCode::Unsupported, "an accessor with no buffer view");
    }
    const JsonRef views = json.member(json.root(), "bufferViews");
    const JsonRef view = json.at(views, static_cast<usize>(json.integer_or(view_ref, -1)));
    if (view == JsonDocument::kNone) {
        return fail(ErrorCode::InvalidArgument,
                    "an accessor naming a buffer view that is not there");
    }
    const auto buffer_index = static_cast<usize>(json.integer_or(json.member(view, "buffer"), -1));
    if (buffer_index >= document.buffers.size()) {
        return fail(ErrorCode::InvalidArgument, "a buffer view naming a buffer that is not there");
    }
    const Span<const u8> buffer = document.buffers[buffer_index];
    const auto view_offset =
        static_cast<usize>(json.integer_or(json.member(view, "byteOffset"), 0));
    const auto view_length =
        static_cast<usize>(json.integer_or(json.member(view, "byteLength"), 0));
    const auto declared_stride =
        static_cast<usize>(json.integer_or(json.member(view, "byteStride"), 0));
    const auto accessor_offset =
        static_cast<usize>(json.integer_or(json.member(accessor, "byteOffset"), 0));
    const usize stride =
        declared_stride != 0 ? declared_stride : static_cast<usize>(element) * lanes;

    if (view_offset + view_length > buffer.size()) {
        return fail(ErrorCode::OutOfRange, "a buffer view past the end of its buffer");
    }
    const usize needed =
        accessor_offset + ((count - 1) * stride) + (static_cast<usize>(element) * lanes);
    if (needed > view_length) {
        return fail(ErrorCode::OutOfRange, "an accessor past the end of its buffer view");
    }

    std::vector<f32> values(count * lanes, 0.0f);
    const u8* base = buffer.data() + view_offset + accessor_offset;
    for (usize index = 0; index < count; ++index) {
        const u8* element_base = base + (index * stride);
        for (u32 lane = 0; lane < lanes; ++lane) {
            const u8* at = element_base + (static_cast<usize>(lane) * element);
            f32 value = 0.0f;
            switch (component_type) {
                case kComponentFloat:
                    value = get_f32(at);
                    break;
                case kComponentUnsignedByte: {
                    const f32 raw = static_cast<f32>(at[0]);
                    value = normalized ? raw / 255.0f : raw;
                    break;
                }
                case kComponentByte: {
                    const auto raw = static_cast<f32>(static_cast<i8>(at[0]));
                    // The normalised mapping glTF specifies for signed types: -128 and -127 both
                    // map to -1, which is what keeps zero exactly representable.
                    f32 scaled = raw / 127.0f;
                    scaled = scaled < -1.0f ? -1.0f : scaled;
                    value = normalized ? scaled : raw;
                    break;
                }
                case kComponentUnsignedShort: {
                    const auto raw =
                        static_cast<f32>(static_cast<u16>(at[0] | (static_cast<u16>(at[1]) << 8U)));
                    value = normalized ? raw / 65535.0f : raw;
                    break;
                }
                case kComponentShort: {
                    const auto raw =
                        static_cast<f32>(static_cast<i16>(at[0] | (static_cast<u16>(at[1]) << 8U)));
                    f32 scaled = raw / 32767.0f;
                    scaled = scaled < -1.0f ? -1.0f : scaled;
                    value = normalized ? scaled : raw;
                    break;
                }
                case kComponentUnsignedInt:
                    value = static_cast<f32>(get_u32(at));
                    break;
                default:
                    return fail(ErrorCode::Unsupported,
                                "an accessor component type glTF does not define");
            }
            values[(index * lanes) + lane] = value;
        }
    }
    return values;
}

/// Read an index accessor into u32.
[[nodiscard]] Expected<std::vector<u32>, Error> read_accessor_indices(const Document& document,
                                                                      JsonRef accessor) noexcept {
    Expected<std::vector<f32>, Error> floats = read_accessor_floats(document, accessor);
    if (!floats) {
        return make_unexpected(floats.error());
    }
    std::vector<u32> indices;
    indices.reserve(floats.value().size());
    for (const f32 value : floats.value()) {
        indices.push_back(static_cast<u32>(value));
    }
    return indices;
}

}  // namespace

// --- The cooked mesh payload ---------------------------------------------------------------------

Status write_cooked_mesh(const MeshData& mesh, Array<u8>& out) noexcept {
    if (Status valid = mesh.validate(); !valid) {
        return valid;
    }
    MeshAttributes attributes = MeshAttributes::None;
    if (!mesh.normals.empty()) {
        attributes = attributes | MeshAttributes::Normals;
    }
    if (!mesh.uvs.empty()) {
        attributes = attributes | MeshAttributes::TexCoords;
    }
    if (!mesh.uv2.empty()) {
        attributes = attributes | MeshAttributes::TexCoords2;
    }
    if (!mesh.tangents.empty()) {
        attributes = attributes | MeshAttributes::Tangents;
    }
    if (!mesh.skin.empty()) {
        attributes = attributes | MeshAttributes::Skin;
    }

    if (Status reserved =
            out.reserve(out.size() + 32 + (mesh.vertex_count() * 72) + (mesh.indices.size() * 4));
        !reserved) {
        return reserved;
    }
    put_u32(out, kCookedMeshVersion);
    put_u32(out, static_cast<u32>(attributes));
    put_u32(out, static_cast<u32>(mesh.vertex_count()));
    put_u32(out, static_cast<u32>(mesh.indices.size()));
    put_u32(out, static_cast<u32>(mesh.sections.size()));

    const Aabb bounds = mesh.bounds();
    // The bounds go in the header rather than being recomputed on load: the culler needs them on
    // every frame and the loader has them for free here.
    const Vec3 lo = mesh.vertex_count() == 0 ? Vec3{} : bounds.min;
    const Vec3 hi = mesh.vertex_count() == 0 ? Vec3{} : bounds.max;
    put_f32(out, lo.x);
    put_f32(out, lo.y);
    put_f32(out, lo.z);
    put_f32(out, hi.x);
    put_f32(out, hi.y);
    put_f32(out, hi.z);

    for (const Vec3& position : mesh.positions) {
        put_f32(out, position.x);
        put_f32(out, position.y);
        put_f32(out, position.z);
    }
    for (const Vec3& normal : mesh.normals) {
        put_f32(out, normal.x);
        put_f32(out, normal.y);
        put_f32(out, normal.z);
    }
    for (const Vec2& uv : mesh.uvs) {
        put_f32(out, uv.x);
        put_f32(out, uv.y);
    }
    for (const Vec2& uv : mesh.uv2) {
        put_f32(out, uv.x);
        put_f32(out, uv.y);
    }
    for (const Vec4& tangent : mesh.tangents) {
        put_f32(out, tangent.x);
        put_f32(out, tangent.y);
        put_f32(out, tangent.z);
        put_f32(out, tangent.w);
    }
    // Joints then weights, both per vertex, in the slot order the influence holds them. Indices
    // are 16-bit because `kMaxSkeletonJoints` is 256 and a pose mask cannot address more.
    for (const SkinInfluence& influence : mesh.skin) {
        for (const u16 joint : influence.joints) {
            put_u16(out, joint);
        }
        for (const f32 weight : influence.weights) {
            put_f32(out, weight);
        }
    }
    for (const u32 index : mesh.indices) {
        put_u32(out, index);
    }
    for (const MeshSection& section : mesh.sections) {
        put_u32(out, section.first_index);
        put_u32(out, section.index_count);
        put_u32(out, section.material);
    }
    return ok();
}

/// Read `count` elements through `read`, appending each to `out` and advancing `cursor`.
///
/// EXTRACTED AT M11.b, when the skin binding made `read_cooked_mesh` the sixth copy of one loop.
/// Six copies of "for each vertex, decode, advance the cursor, push, check" is six places for the
/// cursor arithmetic to be wrong in, and the last one added put the function over this tree's
/// complexity band for a parser. The shape is now stated once and the element decode is what
/// differs, which is the only thing that ever did.
template <class T, class Read>
[[nodiscard]] Status read_elements(Span<const u8> payload, usize& cursor, usize count, usize stride,
                                   Read read, Array<T>& out) noexcept {
    if (Status reserved = out.reserve(count); !reserved) {
        return reserved;
    }
    for (usize index = 0; index < count; ++index) {
        if (Status pushed = out.push_back(read(payload.data() + cursor)); !pushed) {
            return pushed;
        }
        cursor += stride;
    }
    return ok();
}

Status read_cooked_mesh(Span<const u8> payload, MeshData& out) noexcept {
    constexpr usize kHeaderBytes = 44;
    if (payload.size() < kHeaderBytes) {
        return fail(ErrorCode::InvalidArgument, "shorter than a cooked mesh header");
    }
    if (get_u32(payload.data()) != kCookedMeshVersion) {
        return fail(ErrorCode::Unsupported, "a cooked mesh from another format version");
    }
    const auto attributes = static_cast<MeshAttributes>(get_u32(payload.data() + 4));
    const usize vertices = get_u32(payload.data() + 8);
    const usize indices = get_u32(payload.data() + 12);
    const usize sections = get_u32(payload.data() + 16);

    usize needed = kHeaderBytes + (vertices * 12);
    needed += has(attributes, MeshAttributes::Normals) ? vertices * 12 : 0;
    needed += has(attributes, MeshAttributes::TexCoords) ? vertices * 8 : 0;
    needed += has(attributes, MeshAttributes::TexCoords2) ? vertices * 8 : 0;
    needed += has(attributes, MeshAttributes::Tangents) ? vertices * 16 : 0;
    needed += has(attributes, MeshAttributes::Skin) ? vertices * kSkinInfluences * 6 : 0;
    needed += indices * 4;
    needed += sections * 12;
    if (payload.size() != needed) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked mesh whose payload does not match its header");
    }

    out.clear();
    usize cursor = kHeaderBytes;
    const auto vec3_at = [](const u8* at) noexcept {
        return Vec3{get_f32(at), get_f32(at + 4), get_f32(at + 8)};
    };
    const auto vec2_at = [](const u8* at) noexcept { return Vec2{get_f32(at), get_f32(at + 4)}; };
    const auto vec4_at = [](const u8* at) noexcept {
        return Vec4{get_f32(at), get_f32(at + 4), get_f32(at + 8), get_f32(at + 12)};
    };
    const auto influence_at = [](const u8* at) noexcept {
        SkinInfluence influence;
        for (usize slot = 0; slot < kSkinInfluences; ++slot) {
            influence.joints[slot] = get_u16(at + (slot * 2));
            influence.weights[slot] = get_f32(at + (kSkinInfluences * 2) + (slot * 4));
        }
        return influence;
    };

    // The arrays in the order `write_cooked_mesh` wrote them. An attribute the header does not
    // claim contributes nothing and advances the cursor by nothing, which is why the count rather
    // than the block is what the flag controls.
    if (Status read = read_elements(payload, cursor, vertices, 12, vec3_at, out.positions); !read) {
        return read;
    }
    const usize normals = has(attributes, MeshAttributes::Normals) ? vertices : 0;
    if (Status read = read_elements(payload, cursor, normals, 12, vec3_at, out.normals); !read) {
        return read;
    }
    const usize uvs = has(attributes, MeshAttributes::TexCoords) ? vertices : 0;
    if (Status read = read_elements(payload, cursor, uvs, 8, vec2_at, out.uvs); !read) {
        return read;
    }
    const usize uv2 = has(attributes, MeshAttributes::TexCoords2) ? vertices : 0;
    if (Status read = read_elements(payload, cursor, uv2, 8, vec2_at, out.uv2); !read) {
        return read;
    }
    const usize tangents = has(attributes, MeshAttributes::Tangents) ? vertices : 0;
    if (Status read = read_elements(payload, cursor, tangents, 16, vec4_at, out.tangents); !read) {
        return read;
    }
    const usize skin = has(attributes, MeshAttributes::Skin) ? vertices : 0;
    if (Status read =
            read_elements(payload, cursor, skin, kSkinInfluences * 6, influence_at, out.skin);
        !read) {
        return read;
    }
    for (usize index = 0; index < indices; ++index) {
        if (Status pushed = out.indices.push_back(get_u32(payload.data() + cursor)); !pushed) {
            return pushed;
        }
        cursor += 4;
    }
    for (usize index = 0; index < sections; ++index) {
        MeshSection section;
        section.first_index = get_u32(payload.data() + cursor);
        section.index_count = get_u32(payload.data() + cursor + 4);
        section.material = get_u32(payload.data() + cursor + 8);
        cursor += 12;
        if (Status pushed = out.sections.push_back(section); !pushed) {
            return pushed;
        }
    }
    return out.validate();
}

// --- The cooked scene graph ----------------------------------------------------------------------

Status write_cooked_scene_graph(Span<const ImportedNode> nodes, Array<u8>& out) noexcept {
    put_u32(out, kCookedSceneGraphVersion);
    put_u32(out, static_cast<u32>(nodes.size()));
    for (const ImportedNode& node : nodes) {
        if (node.name.size() > 0xFFFFU) {
            return fail(ErrorCode::OutOfRange, "a node name longer than the record allows");
        }
        put_u32(out, static_cast<u32>(node.name.size()));
        if (Status appended = out.append(
                Span<const u8>(reinterpret_cast<const u8*>(node.name.data()), node.name.size()));
            !appended) {
            return appended;
        }
        put_u32(out, static_cast<u32>(node.parent));
        put_f32(out, node.translation.x);
        put_f32(out, node.translation.y);
        put_f32(out, node.translation.z);
        put_f32(out, node.rotation.x);
        put_f32(out, node.rotation.y);
        put_f32(out, node.rotation.z);
        put_f32(out, node.rotation.w);
        put_f32(out, node.scale.x);
        put_f32(out, node.scale.y);
        put_f32(out, node.scale.z);
        put_u32(out, static_cast<u32>(node.mesh));
        put_u32(out, static_cast<u32>(node.collision));
        put_u32(out, node.collision_only ? 1U : 0U);
    }
    return ok();
}

Status read_cooked_scene_graph(Span<const u8> payload, Array<ImportedNode>& out_nodes,
                               Array<char>& out_names) noexcept {
    if (payload.size() < 8) {
        return fail(ErrorCode::InvalidArgument, "shorter than a cooked scene graph header");
    }
    if (get_u32(payload.data()) != kCookedSceneGraphVersion) {
        return fail(ErrorCode::Unsupported, "a cooked scene graph from another format version");
    }
    const usize count = get_u32(payload.data() + 4);
    usize cursor = 8;

    // Two passes: the names first, so `out_names` never reallocates while node views point into it.
    // A single pass would build views that dangle the moment the blob grew, which is the defect
    // this shape exists to prevent.
    struct Slot {
        u32 offset;
        u32 length;
        usize record;
    };
    std::vector<Slot> slots;
    slots.reserve(count);
    usize scan = cursor;
    for (usize index = 0; index < count; ++index) {
        if (scan + 4 > payload.size()) {
            return fail(ErrorCode::InvalidArgument, "a scene graph that ends inside a node");
        }
        const u32 length = get_u32(payload.data() + scan);
        scan += 4;
        if (scan + length + 56 > payload.size()) {
            return fail(ErrorCode::InvalidArgument, "a scene graph that ends inside a node");
        }
        slots.push_back(Slot{static_cast<u32>(out_names.size()), length, scan});
        if (Status appended = out_names.append(
                Span<const char>(reinterpret_cast<const char*>(payload.data() + scan), length));
            !appended) {
            return appended;
        }
        scan += length + 56;
    }

    for (usize index = 0; index < count; ++index) {
        const Slot& slot = slots[index];
        usize at = slot.record + slot.length;
        ImportedNode node;
        node.name = std::string_view(out_names.data() + slot.offset, slot.length);
        node.parent = static_cast<i32>(get_u32(payload.data() + at));
        at += 4;
        node.translation = Vec3{get_f32(payload.data() + at), get_f32(payload.data() + at + 4),
                                get_f32(payload.data() + at + 8)};
        at += 12;
        node.rotation = Quat{get_f32(payload.data() + at), get_f32(payload.data() + at + 4),
                             get_f32(payload.data() + at + 8), get_f32(payload.data() + at + 12)};
        at += 16;
        node.scale = Vec3{get_f32(payload.data() + at), get_f32(payload.data() + at + 4),
                          get_f32(payload.data() + at + 8)};
        at += 12;
        node.mesh = static_cast<i32>(get_u32(payload.data() + at));
        node.collision = static_cast<i32>(get_u32(payload.data() + at + 4));
        node.collision_only = get_u32(payload.data() + at + 8) != 0;
        if (Status pushed = out_nodes.push_back(node); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- The importer --------------------------------------------------------------------------------

OptionsSchema gltf_options() noexcept {
    return OptionsSchema(Span<const OptionSpec>(kGltfOptions));
}

ImporterInfo GltfImporter::info() const noexcept {
    ImporterInfo info;
    // Moved at M6: the overdraw reorder is on by default, the material record is
    // `model.h`'s shared writer, and the schema gained four options. Every glTF
    // re-cooks, which is exactly what a version is for.
    info.name = "gltf";
    // 3 at M11.b: skins, skeletons and animations import, the cooked mesh carries a skin binding,
    // and the cooked mesh record moved to version 2. Every glTF re-cooks, which is what a version
    // is for.
    info.version = 3;
    info.extensions = Span<const std::string_view>(kExtensions);
    info.produces = Span<const assets::AssetKind>(kProduces);
    info.description =
        "Imports a glTF 2.0 file — .gltf or .glb — into cooked meshes with levels of detail, "
        "materials, collision proxies from a naming convention, and the node hierarchy the cook "
        "step turns into a prefab.";
    // NINE OR TEN OF THE TEN, AND THE SET IS A PROPERTY OF THE BUILD. M11.b task 6.1 added steps
    // 7 and 8 to this importer; step 8 needs `cy::animation`'s clip codec, which
    // `-D CY_ANIMATION=OFF` removes, so a build without it reaches nine and says so. `info.steps`
    // participates in the derivation key, so a cache entry written by a build that could not
    // compress a clip is not served to one that can — which is the spec delta this rung carries
    // ("What a cook could not do is part of its derivation key").
    info.steps = static_cast<ModelImportStepSet>(kHierarchyModelSteps |
                                                 step_bit(ModelImportStep::Skeletons));
#ifdef CY_IMPORT_ANIMATION
    info.steps =
        static_cast<ModelImportStepSet>(info.steps | step_bit(ModelImportStep::Animations));
#endif
    return info;
}

OptionsSchema GltfImporter::schema() const noexcept {
    return gltf_options();
}

namespace {

/// Everything `import` needs to carry between its steps, so that the function itself reads as the
/// ordered list of steps `asset-import-pipeline` specifies rather than as one long body.
struct ImportState {
    OptionsSchema options;
    f32 scale = 1.0f;
    bool z_up = false;
    bool import_meshes = true;
    bool import_materials = true;
    /// Steps 7 and 8. M11.b task 6.1.
    bool import_skins = true;
    bool import_animations = true;
    f32 animation_sample_rate = 30.0f;
    f32 animation_translation_tolerance_mm = 0.1f;
    f32 animation_rotation_tolerance_degrees = 0.1f;
    /// Everything the shared steps read. See model.h: the two model importers declare these under
    /// the same option names on purpose, so a project that re-exports a model in the other format
    /// keeps its settings.
    ModelBuildOptions build;
};

/// Convert a position or a translation from the source's conventions into the engine's.
///
/// The engine is right-handed, Y-up, and its unit is the metre (`core-math`). A z-up source is
/// rotated by -90 degrees about X, which sends +Z to +Y and +Y to -Z. This is the whole of "no
/// runtime code accounts for source handedness": it happens here and nowhere else.
[[nodiscard]] Vec3 to_engine(const ImportState& state, Vec3 value) noexcept {
    const Vec3 oriented = state.z_up ? Vec3{value.x, value.z, -value.y} : value;
    return oriented * state.scale;
}

/// The same conversion for a direction, which takes the orientation and not the scale.
[[nodiscard]] Vec3 direction_to_engine(const ImportState& state, Vec3 value) noexcept {
    return state.z_up ? Vec3{value.x, value.z, -value.y} : value;
}

// --- Steps 7 and 8: skins, skeletons and animation clips -----------------------------------------
//
// `asset-import-pipeline` — "Model import", step 7 ("Import skeletons, derive bone LOD levels, and
// remap to a `SkeletonProfile` if configured") and step 8 ("Import animation clips, resample or
// preserve keys per configuration, and compress with error bounds").
//
// UNTIL M11.b THIS IMPORTER REFUSED BOTH BY NAME. A file carrying `skins` or `animations` got one
// `skipped-rig` warning saying "animation-and-skinning reaches Working at M8 and there is nothing
// to import a skeleton into", and its rig was dropped. There has been something to import into
// since M8.b, and M8.d built the FBX half of these two steps; this is the glTF half, written over
// the SAME records — `ImportedSkeleton` from cy/import/fbx_skeleton.h and the cooked clip writer
// from cy/import/clip_record.h. That is `model.h`'s argument applied to rigs: one character
// exported as FBX and as glTF must cook to one skeleton record and one clip record, or every
// downstream `AssetId` rebinds when an artist changes exporter.
//
// WHAT THE JOINT INDICES MEAN, AND WHY THERE ARE TWO NUMBERINGS. A glTF skin owns a `joints` array
// and a mesh's `JOINTS_0` addresses a SLOT in it. `Skeleton::add_joint` refuses a parent index that
// is not strictly smaller than the child's, and glTF requires no such ordering of `joints` — nor
// does it require a skin to list the ancestors whose transforms its joints' bind poses depend on.
// So the cooked skeleton is built by a depth-first walk that puts a parent first and that includes
// every ancestor, and `GltfRig::slot_to_joint` is the map from the file's numbering to the
// record's. Every influence read from a mesh goes through it. Getting this wrong is not a crash: it
// is a character whose left arm moves when its right leg does.

/// The stem of a source path: the file's own name without its directory or its extension.
///
/// It is what an artist typed when they exported the file, and — see `fbx_clip.h` — it is the only
/// thing in a character-library export that distinguishes one animation from another, since every
/// one of them may carry the exporter's own name.
[[nodiscard]] std::string_view stem_of_path(std::string_view path) noexcept {
    const usize slash = path.find_last_of("/\\");
    std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const usize dot = name.rfind('.');
    return dot == std::string_view::npos ? name : name.substr(0, dot);
}

/// A joint the walk emitted, and the two numberings it joins.
struct GltfRig {
    ImportedSkeleton skeleton;
    /// glTF node index to cooked joint index, or -1 for a node that is not a joint.
    std::vector<i32> joint_of_node;
    /// Per glTF skin, the slot-to-joint map its meshes' `JOINTS_0` is remapped through.
    std::vector<std::vector<u16>> slot_to_joint;
    /// The joint names, in cooked order, as a clip's record carries them.
    std::vector<std::string_view> joint_names;
    /// True when a skin drove the walk. False means the table was derived from the node hierarchy
    /// so that an animation-only file — a camera move, a prop, a door — still imports, which is the
    /// fallback `fbx_clip.h` describes and for the same reason.
    bool from_skins = false;
    /// Set when the rig addresses more joints than a pose mask holds. Nothing is produced then.
    bool too_many_joints = false;
};

/// The part of a joint name after the last ':'. `fbx_skeleton.cpp`'s rule, restated here because
/// glTF exporters carry the same namespaces through — a Mixamo rig round-tripped to glTF is still
/// `mixamorig:Hips`.
[[nodiscard]] std::string_view strip_joint_namespace(std::string_view name) noexcept {
    const usize colon = name.rfind(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

/// The parent of every node, by node index, or -1.
///
/// glTF states parentage on the PARENT — a node lists its `children` — so the inverse is built
/// once here. A node named as a child by two parents keeps the first, which is the only
/// deterministic answer to a malformed file.
void build_node_parents(const JsonDocument& json, JsonRef node_array,
                        std::vector<i32>& out) noexcept {
    const usize count = json.size(node_array);
    out.assign(count, -1);
    for (usize index = 0; index < count; ++index) {
        const JsonRef children = json.member(json.at(node_array, index), "children");
        for (usize slot = 0; slot < json.size(children); ++slot) {
            const i64 child = json.integer_or(json.at(children, slot), -1);
            if (child >= 0 && std::cmp_less(child, count) && out[static_cast<usize>(child)] < 0) {
                out[static_cast<usize>(child)] = static_cast<i32>(index);
            }
        }
    }
}

/// The roots to walk from: the scene's nodes when there is a scene, every parentless node
/// otherwise. The same rule step 10 uses, so the two walks cannot disagree about what a root is.
void gltf_walk_roots(const JsonDocument& json, JsonRef root, const std::vector<i32>& parents,
                     std::vector<i32>& out) {
    out.clear();
    const JsonRef scenes = json.member(root, "scenes");
    const auto scene_index = static_cast<usize>(json.integer_or(json.member(root, "scene"), 0));
    const JsonRef scene_roots = json.member(json.at(scenes, scene_index), "nodes");
    for (usize index = 0; index < json.size(scene_roots); ++index) {
        out.push_back(static_cast<i32>(json.integer_or(json.at(scene_roots, index), -1)));
    }
    if (!out.empty()) {
        return;
    }
    for (usize index = 0; index < parents.size(); ++index) {
        if (parents[index] < 0) {
            out.push_back(static_cast<i32>(index));
        }
    }
}

/// Mark every node any skin lists as a joint, and every ancestor of one.
///
/// THE ANCESTORS ARE NOT OPTIONAL. An ancestor's transform is part of its descendants' bind pose,
/// so a skeleton that dropped it would place the whole rig somewhere else — `fbx_skeleton.h` states
/// the same rule for the same reason. A skin whose `joints` array already lists them adds nothing.
[[nodiscard]] bool mark_skin_joints(const JsonDocument& json, JsonRef root,
                                    const std::vector<i32>& parents, std::vector<bool>& marked) {
    marked.assign(parents.size(), false);
    const JsonRef skins = json.member(root, "skins");
    bool any = false;
    for (usize index = 0; index < json.size(skins); ++index) {
        const JsonRef joints = json.member(json.at(skins, index), "joints");
        for (usize slot = 0; slot < json.size(joints); ++slot) {
            i64 node = json.integer_or(json.at(joints, slot), -1);
            while (node >= 0 && std::cmp_less(node, parents.size()) &&
                   !marked[static_cast<usize>(node)]) {
                marked[static_cast<usize>(node)] = true;
                any = true;
                node = parents[static_cast<usize>(node)];
            }
        }
    }
    return any;
}

/// One joint's rest placement, in its parent's space.
///
/// A glTF node's own TRS IS the bind pose: the specification requires the joint nodes to be in
/// their rest placement, and `inverseBindMatrices` is the derived global inverse of exactly that.
/// Reading the TRS rather than inverting the matrices is the choice `fbx_skeleton.h` made and for
/// its reason — it is the identical field step 10 reads for the node table, so the skeleton and the
/// hierarchy cannot disagree about where a joint is. A file whose two disagree is REPORTED rather
/// than silently preferred one way: see `check_inverse_bind`.
[[nodiscard]] Transform joint_bind_local(const JsonDocument& json, JsonRef source_node,
                                         const ImportState& state) noexcept {
    Transform bind;
    const JsonRef matrix = json.member(source_node, "matrix");
    if (json.size(matrix) == 16) {
        Vec3 columns[3];
        for (usize column = 0; column < 3; ++column) {
            columns[column] =
                Vec3{static_cast<f32>(json.number_or(json.at(matrix, (column * 4) + 0), 0.0)),
                     static_cast<f32>(json.number_or(json.at(matrix, (column * 4) + 1), 0.0)),
                     static_cast<f32>(json.number_or(json.at(matrix, (column * 4) + 2), 0.0))};
        }
        bind.scale = Vec3{length(columns[0]), length(columns[1]), length(columns[2])};
        bind.rotation = Quat::from_basis(normalized_or(columns[0], Vec3{1, 0, 0}),
                                         normalized_or(columns[1], Vec3{0, 1, 0}),
                                         normalized_or(columns[2], Vec3{0, 0, 1}));
        bind.translation =
            to_engine(state, Vec3{static_cast<f32>(json.number_or(json.at(matrix, 12), 0.0)),
                                  static_cast<f32>(json.number_or(json.at(matrix, 13), 0.0)),
                                  static_cast<f32>(json.number_or(json.at(matrix, 14), 0.0))});
        return bind;
    }
    const JsonRef translation = json.member(source_node, "translation");
    bind.translation =
        to_engine(state, Vec3{static_cast<f32>(json.number_or(json.at(translation, 0), 0.0)),
                              static_cast<f32>(json.number_or(json.at(translation, 1), 0.0)),
                              static_cast<f32>(json.number_or(json.at(translation, 2), 0.0))});
    const JsonRef rotation = json.member(source_node, "rotation");
    bind.rotation = Quat{static_cast<f32>(json.number_or(json.at(rotation, 0), 0.0)),
                         static_cast<f32>(json.number_or(json.at(rotation, 1), 0.0)),
                         static_cast<f32>(json.number_or(json.at(rotation, 2), 0.0)),
                         static_cast<f32>(json.number_or(json.at(rotation, 3), 1.0))};
    const JsonRef node_scale = json.member(source_node, "scale");
    bind.scale = Vec3{static_cast<f32>(json.number_or(json.at(node_scale, 0), 1.0)),
                      static_cast<f32>(json.number_or(json.at(node_scale, 1), 1.0)),
                      static_cast<f32>(json.number_or(json.at(node_scale, 2), 1.0))};
    return bind;
}

/// Walk the hierarchy depth first, parent before child, emitting every marked node as a joint.
///
/// Deterministic by construction: roots in the scene's own order, children in the file's own order.
/// Nothing here reads a container whose ordering the format leaves open.
void walk_gltf_joints(const JsonDocument& json, JsonRef root, const std::vector<bool>& marked,
                      const std::vector<i32>& parents,
                      const std::vector<std::string_view>& node_names, const ImportState& state,
                      GltfRig& rig) {
    const JsonRef node_array = json.member(root, "nodes");
    rig.joint_of_node.assign(parents.size(), -1);
    std::vector<i32> roots;
    gltf_walk_roots(json, root, parents, roots);

    std::vector<i32> stack;
    for (usize index = roots.size(); index > 0; --index) {
        stack.push_back(roots[index - 1]);
    }
    while (!stack.empty()) {
        const i32 node = stack.back();
        stack.pop_back();
        if (node < 0 || std::cmp_greater_equal(node, parents.size())) {
            continue;
        }
        const auto at = static_cast<usize>(node);
        if (marked[at]) {
            const i32 parent_node = parents[at];
            const i32 parent_joint =
                parent_node >= 0 ? rig.joint_of_node[static_cast<usize>(parent_node)] : -1;
            ImportedJoint joint;
            joint.name = std::string(node_names[at]);
            joint.parent = parent_joint;
            joint.bind_local = joint_bind_local(json, json.at(node_array, at), state);
            joint.dropped_at = joint_bone_lod(joint.name);

            const auto index = static_cast<i32>(rig.skeleton.joints.size());
            // FIRST IN WALK ORDER WINS, exactly as the FBX walk decides it: a rig with two nodes
            // named `LeftHand` has one of them playing the part, and which one is decided by the
            // file's hierarchy rather than by whichever the loop reached last.
            const u16 standard = humanoid_joint_of(joint.name);
            if (standard != kUnmappedHumanoidJoint && rig.skeleton.humanoid.resolve(standard) < 0) {
                rig.skeleton.humanoid.map(standard, index);
            }
            rig.joint_of_node[at] = index;
            rig.skeleton.joints.push_back(std::move(joint));
        }
        const JsonRef children = json.member(json.at(node_array, at), "children");
        for (usize child = json.size(children); child > 0; --child) {
            stack.push_back(static_cast<i32>(json.integer_or(json.at(children, child - 1), -1)));
        }
    }
}

/// Make the bone levels nested subsets, which `Skeleton::finalize()` refuses a skeleton for not
/// being. One forward pass suffices precisely because the joints are parent-before-child.
void clamp_gltf_bone_levels(ImportedSkeleton& skeleton) noexcept {
    for (usize index = 0; index < skeleton.joints.size(); ++index) {
        const i32 parent = skeleton.joints[index].parent;
        if (parent >= 0) {
            const u8 limit = skeleton.joints[static_cast<usize>(parent)].dropped_at;
            skeleton.joints[index].dropped_at = std::min(skeleton.joints[index].dropped_at, limit);
        }
    }
}

/// How far a joint's derived global bind pose is from the inverse the file supplied, as the largest
/// absolute departure of `global * inverseBind` from the identity.
///
/// A CHECK RATHER THAN A CONVERSION, and the distinction is the point. glTF's own specification
/// makes the node TRS and `inverseBindMatrices` two statements of one fact, so on a well-formed
/// file this number is a rounding error. When it is not — a rig exported in a posed state, a tool
/// that baked one and not the other — the rig this importer produces is not the rig the file meant,
/// and the import must SAY so. Silently preferring either source is how a character comes in with
/// its arms in the wrong place and no diagnostic anywhere.
[[nodiscard]] f32 inverse_bind_deviation(const Mat4& global, const Mat4& inverse_bind) noexcept {
    const Mat4 product = global * inverse_bind;
    f32 worst = 0.0f;
    for (usize row = 0; row < 4; ++row) {
        for (usize column = 0; column < 4; ++column) {
            const f32 expected = row == column ? 1.0f : 0.0f;
            const f32 departure = std::abs(product.at(row, column) - expected);
            worst = departure > worst ? departure : worst;
        }
    }
    return worst;
}

/// Build the file's one skeleton, its slot maps and its joint-name table.
///
/// ONE SKELETON PER FILE AND NOT ONE PER SKIN, which is the shape the FBX importer already has: a
/// character with a separate skin for its body and its coat is one rig, and two records would be
/// two rigs a clip could only be bound to one of. Each skin keeps its own slot map into that one
/// record.
///
/// Fills `rig` whether or not a sub-asset is emitted, so step 8 can index its tracks against the
/// same joint numbering.
[[nodiscard]] Status build_gltf_rig(const JsonDocument& json, JsonRef root,
                                    const ImportState& state,
                                    const std::vector<std::string_view>& node_names,
                                    const std::vector<i32>& parents, ImportResult& out,
                                    std::string_view source_path, GltfRig& rig) noexcept {
    std::vector<bool> marked;
    rig.from_skins = mark_skin_joints(json, root, parents, marked);
    if (!rig.from_skins) {
        // NO SKIN IS NOT AN ERROR. An animation-only export — a camera move, a prop, a door — has
        // no rig and still has motion worth importing, so the table is every node. It is the
        // fallback `fbx_clip.h` describes, and it is what keeps step 8 honest when step 7 declines
        // to produce a skeleton.
        marked.assign(parents.size(), true);
    }
    walk_gltf_joints(json, root, marked, parents, node_names, state, rig);
    if (rig.skeleton.joints.empty()) {
        return ok();
    }
    if (rig.skeleton.joints.size() >= kMaxSkeletonJoints) {
        // `graph::pose::kMaxJoints` is a fixed `u64[4]` mask, so a rig over the cap could never be
        // bound to a pose. Truncating a hierarchy silently is worse than not importing it.
        rig.too_many_joints = true;
        rig.skeleton.joints.clear();
        rig.joint_of_node.assign(parents.size(), -1);
        char detail[ImportDiagnostic::kDetailCapacity] = {};
        (void)std::snprintf(detail, sizeof(detail),
                            "this rig addresses more joints than a pose mask holds, so neither a "
                            "skeleton nor any animation was imported from it");
        return out.report(ImportSeverity::Warning, "rig-too-large", detail, source_path);
    }
    clamp_gltf_bone_levels(rig.skeleton);

    // `Root` answers with the first root joint when the rig has no dedicated one, which is the case
    // a Mixamo hierarchy is — its hips ARE the root. `SkeletonProfile` allows two standard joints
    // to resolve to one, which is why this is a side table rather than a field.
    if (rig.skeleton.humanoid.resolve(0) < 0) {
        for (usize index = 0; index < rig.skeleton.joints.size(); ++index) {
            if (rig.skeleton.joints[index].parent < 0) {
                rig.skeleton.humanoid.map(0, static_cast<i32>(index));
                break;
            }
        }
    }

    rig.joint_names.reserve(rig.skeleton.joints.size());
    for (const ImportedJoint& joint : rig.skeleton.joints) {
        rig.joint_names.emplace_back(joint.name);
    }

    const JsonRef skins = json.member(root, "skins");
    rig.slot_to_joint.resize(json.size(skins));
    for (usize index = 0; index < json.size(skins); ++index) {
        const JsonRef joints = json.member(json.at(skins, index), "joints");
        std::vector<u16>& slots = rig.slot_to_joint[index];
        slots.reserve(json.size(joints));
        for (usize slot = 0; slot < json.size(joints); ++slot) {
            const i64 node = json.integer_or(json.at(joints, slot), -1);
            const i32 joint = node >= 0 && std::cmp_less(node, rig.joint_of_node.size())
                                  ? rig.joint_of_node[static_cast<usize>(node)]
                                  : -1;
            // A slot naming a node the walk never reached — a joint outside every scene — binds to
            // joint 0 rather than out of range. It cannot happen in a well-formed file, and the
            // alternative to a defined answer is a read past the end of a pose.
            slots.push_back(joint >= 0 ? static_cast<u16>(joint) : u16{0});
        }
    }
    return ok();
}

/// One inverse bind matrix out of a `MAT4` accessor's floats, column-major as glTF writes them.
///
/// `scale` is the importer's own option, which multiplies every translation: the file's matrix is
/// in the file's units and the derived bind pose is in the scaled ones, so the comparison has to
/// bring them into the same space or every scaled import would report a disagreement.
[[nodiscard]] Mat4 inverse_bind_at(const std::vector<f32>& values, usize slot, f32 scale) noexcept {
    Mat4 matrix;
    for (usize column = 0; column < 4; ++column) {
        for (usize row = 0; row < 4; ++row) {
            matrix.at(row, column) = values[(slot * 16) + (column * 4) + row];
        }
    }
    for (usize row = 0; row < 3; ++row) {
        matrix.at(row, 3) *= scale;
    }
    return matrix;
}

/// Compare every skin's `inverseBindMatrices` against the bind pose the walk derived, and report
/// the worst departure when it is past what float arithmetic explains.
[[nodiscard]] Status check_inverse_bind(const JsonDocument& json, JsonRef root,
                                        const Document& document, const ImportState& state,
                                        const GltfRig& rig, ImportResult& out,
                                        std::string_view source_path) noexcept {
    if (rig.skeleton.joints.empty()) {
        return ok();
    }
    // The global bind of every joint, composed once from the parent-before-child order.
    std::vector<Mat4> global(rig.skeleton.joints.size());
    for (usize index = 0; index < rig.skeleton.joints.size(); ++index) {
        const ImportedJoint& joint = rig.skeleton.joints[index];
        const Mat4 local = joint.bind_local.to_matrix();
        global[index] = joint.parent < 0 ? local : global[static_cast<usize>(joint.parent)] * local;
    }

    const JsonRef skins = json.member(root, "skins");
    const JsonRef accessors = json.member(root, "accessors");
    f32 worst = 0.0f;
    std::string_view worst_joint;
    for (usize index = 0; index < json.size(skins) && index < rig.slot_to_joint.size(); ++index) {
        const JsonRef matrices = json.member(json.at(skins, index), "inverseBindMatrices");
        if (matrices == JsonDocument::kNone) {
            continue;  // Defined to be identity, which says nothing to compare against.
        }
        Expected<std::vector<f32>, Error> values = read_accessor_floats(
            document, json.at(accessors, static_cast<usize>(json.integer_or(matrices, -1))));
        if (!values) {
            continue;
        }
        const std::vector<u16>& slots = rig.slot_to_joint[index];
        for (usize slot = 0; slot < slots.size(); ++slot) {
            if (((slot + 1) * 16) > values.value().size()) {
                break;
            }
            const Mat4 inverse_bind = inverse_bind_at(values.value(), slot, state.scale);
            const f32 deviation = inverse_bind_deviation(global[slots[slot]], inverse_bind);
            if (deviation > worst) {
                worst = deviation;
                worst_joint = rig.skeleton.joints[slots[slot]].name;
            }
        }
    }

    // A thousandth is two orders of magnitude above what composing a dozen 32-bit transforms costs
    // and two orders below anything an eye can see, so it separates arithmetic from disagreement
    // rather than splitting either.
    constexpr f32 kBindTolerance = 1.0e-3f;
    if (worst <= kBindTolerance) {
        return ok();
    }
    char detail[ImportDiagnostic::kDetailCapacity] = {};
    (void)std::snprintf(detail, sizeof(detail),
                        "inverse bind matrices disagree with the joint transforms by %.4f at "
                        "'%.40s'; the rig came from the transforms",
                        static_cast<f64>(worst), std::string(worst_joint).c_str());
    return out.report(ImportSeverity::Warning, "bind-pose-disagrees", detail, source_path);
}

#ifdef CY_IMPORT_ANIMATION

/// What step 8 produced, measured rather than estimated.
struct GltfClipReport {
    u32 animations = 0;
    u32 clips = 0;
    u32 constant = 0;
    u32 tracks = 0;
    u32 keys_before = 0;
    u32 keys_after = 0;
    f32 worst_translation_mm = 0.0f;
    f32 worst_rotation_degrees = 0.0f;
    /// Channels targeting something this import has no joint index for — motion it DROPPED, and
    /// the number that must never be non-zero without a diagnostic.
    u32 unmapped_channels = 0;
    /// Channels targeting `weights`: morph-target animation, which no step of this importer reads.
    u32 morph_channels = 0;
};

/// One channel's samples, read out of its sampler.
struct GltfSampler {
    std::vector<f32> times;
    std::vector<f32> values;
    /// Components per key: 3 for translation and scale, 4 for rotation.
    u32 lanes = 0;
    animation::Interpolation interpolation = animation::Interpolation::Linear;
    /// True for CUBICSPLINE, whose output holds an in-tangent, the value and an out-tangent per
    /// key. Only the value is read: `Interpolation::Cubic` stores linear between the keys the
    /// fitter keeps and carries no tangent, which `clip.h` states as a limitation of the codec
    /// rather than of the format. The tolerance is measured against the authored keys either way,
    /// so a cubic track simply keeps more of them.
    bool cubic = false;
};

[[nodiscard]] bool read_gltf_sampler(const JsonDocument& json, const Document& document,
                                     JsonRef accessors, JsonRef sampler, u32 lanes,
                                     GltfSampler& out) noexcept {
    Expected<std::vector<f32>, Error> times = read_accessor_floats(
        document,
        json.at(accessors, static_cast<usize>(json.integer_or(json.member(sampler, "input"), -1))));
    Expected<std::vector<f32>, Error> values = read_accessor_floats(
        document, json.at(accessors,
                          static_cast<usize>(json.integer_or(json.member(sampler, "output"), -1))));
    if (!times || !values || times.value().empty()) {
        return false;
    }
    const std::string_view mode = json.string_or(json.member(sampler, "interpolation"), "LINEAR");
    out.cubic = mode == "CUBICSPLINE";
    out.lanes = lanes;
    if (mode == "STEP") {
        out.interpolation = animation::Interpolation::Step;
    } else if (out.cubic) {
        out.interpolation = animation::Interpolation::Cubic;
    } else {
        out.interpolation =
            lanes == 4 ? animation::Interpolation::Spherical : animation::Interpolation::Linear;
    }
    const usize stride = out.cubic ? usize{3} * lanes : usize{lanes};
    if (values.value().size() < times.value().size() * stride) {
        return false;
    }
    out.times = std::move(times.value());
    out.values = std::move(values.value());
    return true;
}

/// The key at `index`, with CUBICSPLINE's tangents stepped over.
[[nodiscard]] Vec4 gltf_sampler_value(const GltfSampler& sampler, usize index) noexcept {
    const usize stride = sampler.cubic ? usize{3} * sampler.lanes : usize{sampler.lanes};
    const usize at = (index * stride) + (sampler.cubic ? sampler.lanes : usize{0});
    Vec4 value{0.0f, 0.0f, 0.0f, 0.0f};
    for (usize lane = 0; lane < sampler.lanes; ++lane) {
        value[lane] = sampler.values[at + lane];
    }
    return value;
}

/// Author one channel onto the clip.
///
/// THE ORDER IS FORCED BY THE CLIP, not chosen: `Clip::add_key` refuses any track but the
/// last-added one, because every track's keys live in one shared array. So a track is opened and
/// filled before the next is opened.
[[nodiscard]] Status author_gltf_channel(const GltfSampler& sampler, animation::TrackKind kind,
                                         u16 joint, const ImportState& state,
                                         animation::Clip& clip) noexcept {
    Expected<u32, Error> track = clip.add_joint_track(kind, joint, sampler.interpolation);
    if (!track) {
        return make_unexpected(track.error());
    }
    f32 previous = -1.0f;
    for (usize index = 0; index < sampler.times.size(); ++index) {
        // Keys must arrive in increasing time order, and a source that repeats a time would make
        // the codec's segment search ambiguous. A repeated or decreasing key is dropped rather
        // than nudged, because nudging invents a keyframe nobody authored.
        const f32 time = sampler.times[index];
        if (index != 0 && time <= previous) {
            continue;
        }
        previous = time;
        Vec4 value = gltf_sampler_value(sampler, index);
        if (kind == animation::TrackKind::Translation) {
            const Vec3 converted = to_engine(state, Vec3{value.x, value.y, value.z});
            value = Vec4{converted.x, converted.y, converted.z, 0.0f};
        }
        if (Status added = clip.add_key(track.value(), time, value); !added) {
            return added;
        }
    }
    return ok();
}

/// Import every animation in the file as its own `Animation` sub-asset.
[[nodiscard]] Status import_gltf_animations(const JsonDocument& json, JsonRef root,
                                            const Document& document, const ImportState& state,
                                            const GltfRig& rig, std::string_view source_path,
                                            SubAssetNames& names, ImportResult& out,
                                            GltfClipReport& report) noexcept {
    const JsonRef animations = json.member(root, "animations");
    report.animations = static_cast<u32>(json.size(animations));
    if (report.animations == 0 || rig.skeleton.joints.empty()) {
        return ok();
    }
    const JsonRef accessors = json.member(root, "accessors");
    const std::string_view stem = stem_of_path(source_path);

    for (usize index = 0; index < json.size(animations); ++index) {
        const JsonRef source = json.at(animations, index);
        const JsonRef channels = json.member(source, "channels");
        const JsonRef samplers = json.member(source, "samplers");

        animation::Clip clip(current_allocator());
        const std::string_view declared = json.string_or(json.member(source, "name"), "");
        // ONE ANIMATION TAKES THE FILE'S OWN STEM, SEVERAL TAKE THEIR OWN NAMES — the rule
        // `fbx_clip.h` sets and for its reason: a library export names every clip the same thing,
        // and the file name is what the artist actually chose.
        const std::string_view clip_name =
            report.animations == 1 || declared.empty() ? stem : declared;
        clip.set_name(Name::intern(clip_name));

        // CHANNELS ARE GROUPED BY JOINT BEFORE ANY IS AUTHORED. `add_key` refuses any track but the
        // last opened one, and glTF fixes no ordering on `channels` — a file that interleaves two
        // joints' translations would otherwise import one key each and drop the rest, in silence.
        struct JointChannels {
            u16 joint = 0;
            i32 translation = -1;
            i32 rotation = -1;
            i32 scale = -1;
        };
        std::vector<JointChannels> per_joint;
        f32 duration = 0.0f;
        for (usize slot = 0; slot < json.size(channels); ++slot) {
            const JsonRef channel = json.at(channels, slot);
            const JsonRef target = json.member(channel, "target");
            const i64 node = json.integer_or(json.member(target, "node"), -1);
            const std::string_view path = json.string_or(json.member(target, "path"), "");
            if (path == "weights") {
                ++report.morph_channels;
                continue;
            }
            const i32 joint = node >= 0 && std::cmp_less(node, rig.joint_of_node.size())
                                  ? rig.joint_of_node[static_cast<usize>(node)]
                                  : -1;
            if (joint < 0) {
                ++report.unmapped_channels;
                continue;
            }
            JointChannels* entry = nullptr;
            for (JointChannels& candidate : per_joint) {
                if (candidate.joint == static_cast<u16>(joint)) {
                    entry = &candidate;
                    break;
                }
            }
            if (entry == nullptr) {
                per_joint.push_back(JointChannels{static_cast<u16>(joint), -1, -1, -1});
                entry = &per_joint.back();
            }
            const auto sampler_index =
                static_cast<i32>(json.integer_or(json.member(channel, "sampler"), -1));
            if (path == "translation") {
                entry->translation = sampler_index;
            } else if (path == "rotation") {
                entry->rotation = sampler_index;
            } else if (path == "scale") {
                entry->scale = sampler_index;
            }
        }

        const auto author = [&](i32 sampler_index, animation::TrackKind kind, u16 joint,
                                u32 lanes) noexcept -> Status {
            if (sampler_index < 0) {
                return ok();
            }
            GltfSampler sampler;
            if (!read_gltf_sampler(json, document, accessors,
                                   json.at(samplers, static_cast<usize>(sampler_index)), lanes,
                                   sampler)) {
                return ok();
            }
            duration = sampler.times.back() > duration ? sampler.times.back() : duration;
            return author_gltf_channel(sampler, kind, joint, state, clip);
        };
        for (const JointChannels& entry : per_joint) {
            if (Status authored =
                    author(entry.translation, animation::TrackKind::Translation, entry.joint, 3);
                !authored) {
                return authored;
            }
            if (Status authored =
                    author(entry.rotation, animation::TrackKind::Rotation, entry.joint, 4);
                !authored) {
                return authored;
            }
            if (Status authored = author(entry.scale, animation::TrackKind::Scale, entry.joint, 3);
                !authored) {
                return authored;
            }
        }
        if (clip.track_count() == 0) {
            continue;
        }
        // A clip whose duration is zero divides by it when a time is wrapped, so a single-key
        // animation gets one sample period rather than nothing.
        clip.set_duration(duration > 0.0f ? duration : 1.0f / state.animation_sample_rate);
        clip.set_sample_rate_hint(state.animation_sample_rate);

        animation::CompressionSettings settings;
        settings.translation_tolerance_mm = state.animation_translation_tolerance_mm;
        settings.rotation_tolerance_degrees = state.animation_rotation_tolerance_degrees;
        if (Status compressed = clip.compress(settings); !compressed) {
            return compressed;
        }
        const animation::CompressionReport& measured = clip.report();
        if (measured.tracks == measured.constant_tracks) {
            // Every track collapsed to one key: this animation animates nothing within the
            // tolerances this import declared. Reported, never dropped in silence.
            ++report.constant;
            if (Status reported =
                    out.report(ImportSeverity::Info, "constant-animation",
                               "every track of this animation holds one value for its whole "
                               "length, so it animates nothing and produced no clip",
                               clip_name);
                !reported) {
                return reported;
            }
            continue;
        }

        Array<u8> payload;
        if (Status written = write_cooked_clip(
                clip, Span<const std::string_view>(rig.joint_names.data(), rig.joint_names.size()),
                payload);
            !written) {
            return written;
        }
        const std::string name = names.unique("animation/", clip_name, index);
        if (Status added = out.add(assets::AssetKind::Animation, name, std::move(payload), false);
            !added) {
            return added;
        }
        ++report.clips;
        report.tracks += measured.tracks;
        report.keys_before += measured.keys_before;
        report.keys_after += measured.keys_after;
        report.worst_translation_mm = measured.worst_translation_mm > report.worst_translation_mm
                                          ? measured.worst_translation_mm
                                          : report.worst_translation_mm;
        report.worst_rotation_degrees =
            measured.worst_rotation_degrees > report.worst_rotation_degrees
                ? measured.worst_rotation_degrees
                : report.worst_rotation_degrees;
    }
    return ok();
}

#endif  // CY_IMPORT_ANIMATION

}  // namespace

Status GltfImporter::import(const ImportRequest& request, ImportResult& out) noexcept {
    ImportState state;
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
    Expected<OptionValue, Error> skins_option = option("import-skins");
    Expected<OptionValue, Error> animations_option = option("import-animations");
    Expected<OptionValue, Error> sample_rate = option("animation-sample-rate");
    Expected<OptionValue, Error> translation_tolerance =
        option("animation-translation-tolerance-mm");
    Expected<OptionValue, Error> rotation_tolerance =
        option("animation-rotation-tolerance-degrees");
    if (!scale || !up || !meshes || !materials || !weld_tolerance || !smoothing || !tangents ||
        !optimise_option || !overdraw || !lightmap || !density || !padding || !lod_count ||
        !lod_ratio || !lod_error || !collision_suffix || !collision_mode || !skins_option ||
        !animations_option || !sample_rate || !translation_tolerance || !rotation_tolerance) {
        return fail(ErrorCode::Internal, "the glTF importer's own option schema is inconsistent");
    }
    state.scale = static_cast<f32>(scale.value().as_float());
    state.z_up = up.value().as_text() == "z-up";
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
    state.import_skins = skins_option.value().as_bool();
    state.import_animations = animations_option.value().as_bool();
    state.animation_sample_rate = static_cast<f32>(sample_rate.value().as_float());
    state.animation_translation_tolerance_mm =
        static_cast<f32>(translation_tolerance.value().as_float());
    state.animation_rotation_tolerance_degrees =
        static_cast<f32>(rotation_tolerance.value().as_float());

    // --- 1. Parse.
    std::string_view json_text;
    Span<const u8> binary_chunk;
    if (Status split = split_container(request.bytes, json_text, binary_chunk); !split) {
        return out.report(ImportSeverity::Error, "unreadable-container", split.error().message,
                          request.source.view());
    }
    Expected<JsonDocument, Error> parsed = JsonDocument::parse(json_text);
    if (!parsed) {
        return out.report(ImportSeverity::Error, "malformed-json", parsed.error().message,
                          request.source.view());
    }

    Document document;
    document.json = std::move(parsed.value());
    const JsonDocument& json = document.json;
    const JsonRef root = json.root();
    if (!json.is(root, JsonKind::Object)) {
        return out.report(ImportSeverity::Error, "malformed-json",
                          "the root of a glTF file is an object", request.source.view());
    }

    // --- Resolve the buffers. A data URI is decoded here; an external one is read through the
    // resolver, which is what records it as a dependency — see the note at the head of importer.h.
    const JsonRef buffers = json.member(root, "buffers");
    for (usize index = 0; index < json.size(buffers); ++index) {
        const JsonRef buffer = json.at(buffers, index);
        const std::string_view uri = json.string_or(json.member(buffer, "uri"), "");
        if (uri.empty()) {
            if (binary_chunk.empty()) {
                return out.report(ImportSeverity::Error, "missing-buffer",
                                  "a buffer with no uri and no binary chunk to take it from",
                                  request.source.view());
            }
            document.buffers.push_back(binary_chunk);
            document.owned_buffers.emplace_back();
            continue;
        }
        if (uri.starts_with("data:")) {
            const usize comma = uri.find(',');
            if (comma == std::string_view::npos) {
                return out.report(ImportSeverity::Error, "malformed-uri",
                                  "a data uri with no comma", request.source.view());
            }
            Expected<std::vector<u8>, Error> decoded = decode_base64(uri.substr(comma + 1));
            if (!decoded) {
                return out.report(ImportSeverity::Error, "malformed-uri", decoded.error().message,
                                  request.source.view());
            }
            document.owned_buffers.emplace_back(std::move(decoded.value()));
            document.buffers.emplace_back(document.owned_buffers.back().data(),
                                          document.owned_buffers.back().size());
            continue;
        }
        if (request.resolver == nullptr) {
            return out.report(ImportSeverity::Error, "unresolvable-buffer",
                              "an external buffer and no resolver to read it through",
                              request.source.view());
        }
        Expected<Span<const u8>, Error> bytes = request.resolver->read(uri);
        if (!bytes) {
            return out.report(ImportSeverity::Error, "missing-buffer",
                              "the buffer this file references could not be read", uri);
        }
        document.owned_buffers.emplace_back(bytes.value().begin(), bytes.value().end());
        document.buffers.emplace_back(document.owned_buffers.back().data(),
                                      document.owned_buffers.back().size());
    }

    // --- Textures. They are recorded as dependencies and referenced by path; the texture importer
    // is what imports them. Two importers for one file would mean two cooked textures and two
    // asset ids for the same image.
    const JsonRef images = json.member(root, "images");
    for (usize index = 0; index < json.size(images); ++index) {
        const std::string_view uri = json.string_or(json.member(json.at(images, index), "uri"), "");
        if (uri.empty() || uri.starts_with("data:")) {
            continue;
        }
        if (request.resolver != nullptr) {
            Expected<Span<const u8>, Error> bytes = request.resolver->read(uri);
            if (!bytes) {
                if (Status reported = out.report(
                        ImportSeverity::Warning, "missing-texture",
                        "a texture this model references is not beside it; the material will "
                        "resolve to a placeholder until it is added",
                        uri);
                    !reported) {
                    return reported;
                }
            }
        }
    }

    // --- 7. The skeleton. It runs BEFORE the meshes because a mesh's `JOINTS_0` addresses a slot
    // in its skin's own `joints` array, and the cooked record numbers joints differently — see the
    // section header on `GltfRig` for why the two numberings exist. `rig.slot_to_joint` is what
    // joins them, and it has to exist before the first influence is read.
    const JsonRef rig_nodes = json.member(root, "nodes");
    const usize rig_node_count = json.size(rig_nodes);
    std::vector<std::string_view> rig_node_names;
    rig_node_names.reserve(rig_node_count);
    for (usize index = 0; index < rig_node_count; ++index) {
        rig_node_names.push_back(
            json.string_or(json.member(json.at(rig_nodes, index), "name"), ""));
    }
    std::vector<i32> rig_parents;
    build_node_parents(json, rig_nodes, rig_parents);

    /// The skin each glTF mesh is deformed by, taken from the nodes that draw it, or -1.
    ///
    /// A mesh drawn by two nodes with two different skins is a file this importer cannot represent
    /// — the engine's mesh carries one binding per vertex — so the FIRST is kept and the conflict
    /// is named rather than silently resolved.
    std::vector<i32> skin_of_mesh(json.size(json.member(root, "meshes")), -1);
    bool conflicting_skins = false;
    for (usize index = 0; index < rig_node_count; ++index) {
        const JsonRef source_node = json.at(rig_nodes, index);
        const i64 mesh = json.integer_or(json.member(source_node, "mesh"), -1);
        const i64 skin = json.integer_or(json.member(source_node, "skin"), -1);
        if (mesh < 0 || skin < 0 || std::cmp_greater_equal(mesh, skin_of_mesh.size())) {
            continue;
        }
        i32& slot = skin_of_mesh[static_cast<usize>(mesh)];
        conflicting_skins = conflicting_skins || (slot >= 0 && slot != static_cast<i32>(skin));
        slot = slot >= 0 ? slot : static_cast<i32>(skin);
    }
    if (conflicting_skins) {
        if (Status reported = out.report(
                ImportSeverity::Warning, "mesh-shared-between-skins",
                "a mesh in this file is drawn by nodes with different skins; a cooked mesh carries "
                "one binding per vertex, so the first skin was used for all of them",
                request.source.view());
            !reported) {
            return reported;
        }
    }

    GltfRig rig;
    if (state.import_skins || state.import_animations) {
        if (Status built = build_gltf_rig(json, root, state, rig_node_names, rig_parents, out,
                                          request.source.view(), rig);
            !built) {
            return built;
        }
    }

    // --- 9. Materials.
    //
    // The record is `model.h`'s `StandardMaterial`, written by the one function both model
    // importers write it with: glTF's metallic-roughness and FBX's PBR maps are two spellings of
    // one model, and two records would mean the same asset re-exported cooks to different bytes.
    // Texture references are NOT in it — the pipeline resolves them by `AssetId` through the asset
    // database, which is what makes "a referenced texture moves and no re-import is needed" true.
    SubAssetNames names;
    std::vector<std::string> material_names;
    const JsonRef material_array = json.member(root, "materials");
    for (usize index = 0; index < json.size(material_array); ++index) {
        const JsonRef material = json.at(material_array, index);
        const std::string_view name = json.string_or(json.member(material, "name"), "");
        std::string stable = names.unique("material/", name, index);
        material_names.push_back(stable);
        if (!state.import_materials) {
            continue;
        }
        StandardMaterial record;
        const JsonRef pbr = json.member(material, "pbrMetallicRoughness");
        const JsonRef base_colour = json.member(pbr, "baseColorFactor");
        for (usize lane = 0; lane < 4; ++lane) {
            record.base_colour[lane] =
                static_cast<f32>(json.number_or(json.at(base_colour, lane), 1.0));
        }
        record.metallic = static_cast<f32>(json.number_or(json.member(pbr, "metallicFactor"), 1.0));
        record.roughness =
            static_cast<f32>(json.number_or(json.member(pbr, "roughnessFactor"), 1.0));
        const JsonRef emissive = json.member(material, "emissiveFactor");
        for (usize lane = 0; lane < 3; ++lane) {
            record.emissive[lane] = static_cast<f32>(json.number_or(json.at(emissive, lane), 0.0));
        }
        const std::string_view alpha_mode =
            json.string_or(json.member(material, "alphaMode"), "OPAQUE");
        if (alpha_mode == "BLEND") {
            record.alpha_mode = 2;
        } else if (alpha_mode == "MASK") {
            record.alpha_mode = 1;
        }
        record.alpha_cutoff =
            static_cast<f32>(json.number_or(json.member(material, "alphaCutoff"), 0.5));
        record.double_sided = json.bool_or(json.member(material, "doubleSided"), false);

        Array<u8> payload;
        if (Status written = write_cooked_material(record, payload); !written) {
            return written;
        }
        if (Status added = out.add(assets::AssetKind::Material, stable, std::move(payload), false);
            !added) {
            return added;
        }
    }

    // --- 2 to 6. Meshes.
    //
    // The built meshes are KEPT rather than written and forgotten, because collision is generated
    // from the source mesh — "WHEN a 50-million-triangle asset is cooked THEN its collision
    // representation SHALL be generated from the source mesh ... and SHALL NOT derive from the
    // render clusters" — and the node that names a collider is not read until step 10.
    std::vector<MeshData> built_meshes;
    std::vector<std::string> mesh_names;
    const JsonRef mesh_array = json.member(root, "meshes");
    for (usize index = 0; index < json.size(mesh_array); ++index) {
        const JsonRef source_mesh = json.at(mesh_array, index);
        const std::string_view name = json.string_or(json.member(source_mesh, "name"), "");
        mesh_names.push_back(names.unique("mesh/", name, index));
        built_meshes.emplace_back();
        if (!state.import_meshes) {
            continue;
        }

        MeshData built;
        const JsonRef primitives = json.member(source_mesh, "primitives");
        bool usable = false;
        for (usize slot = 0; slot < json.size(primitives); ++slot) {
            const JsonRef primitive = json.at(primitives, slot);
            const i64 mode = json.integer_or(json.member(primitive, "mode"), 4);
            if (mode != 4) {
                if (Status reported = out.report(
                        ImportSeverity::Warning, "unsupported-primitive-mode",
                        "a primitive that is not a triangle list was skipped; strips, fans, lines "
                        "and points are not imported",
                        mesh_names.back());
                    !reported) {
                    return reported;
                }
                continue;
            }
            const JsonRef attributes = json.member(primitive, "attributes");
            const JsonRef accessors = json.member(root, "accessors");
            const JsonRef position_ref = json.member(attributes, "POSITION");
            if (position_ref == JsonDocument::kNone) {
                continue;
            }
            const JsonRef position_accessor =
                json.at(accessors, static_cast<usize>(json.integer_or(position_ref, -1)));
            Expected<std::vector<f32>, Error> positions =
                read_accessor_floats(document, position_accessor);
            if (!positions) {
                if (Status reported = out.report(ImportSeverity::Error, "unreadable-accessor",
                                                 positions.error().message, mesh_names.back());
                    !reported) {
                    return reported;
                }
                continue;
            }
            const usize vertex_count = positions.value().size() / 3;
            const auto base = static_cast<u32>(built.positions.size());

            for (usize vertex = 0; vertex < vertex_count; ++vertex) {
                const Vec3 raw{positions.value()[(vertex * 3) + 0],
                               positions.value()[(vertex * 3) + 1],
                               positions.value()[(vertex * 3) + 2]};
                if (Status pushed = built.positions.push_back(to_engine(state, raw)); !pushed) {
                    return pushed;
                }
            }

            const auto read_optional =
                [&](std::string_view attribute) noexcept -> std::vector<f32> {
                const JsonRef ref = json.member(attributes, attribute);
                if (ref == JsonDocument::kNone) {
                    return {};
                }
                Expected<std::vector<f32>, Error> values = read_accessor_floats(
                    document, json.at(accessors, static_cast<usize>(json.integer_or(ref, -1))));
                return values ? std::move(values.value()) : std::vector<f32>{};
            };

            const std::vector<f32> normals = read_optional("NORMAL");
            const std::vector<f32> uvs = read_optional("TEXCOORD_0");
            const std::vector<f32> uv2 = read_optional("TEXCOORD_1");
            // Step 7's half that lives on the mesh. `JOINTS_0` is an unsigned byte or short
            // accessor and `WEIGHTS_0` a float or a normalised integer one; `read_accessor_floats`
            // already handles both, including glTF's normalisation rule.
            const bool want_skin = state.import_skins && !rig.skeleton.joints.empty();
            const std::vector<f32> joint_slots =
                want_skin ? read_optional("JOINTS_0") : std::vector<f32>{};
            const std::vector<f32> joint_weights =
                want_skin ? read_optional("WEIGHTS_0") : std::vector<f32>{};
            const bool has_skin = joint_slots.size() == vertex_count * kSkinInfluences &&
                                  joint_weights.size() == vertex_count * kSkinInfluences;
            const std::vector<u16>* slot_map = nullptr;
            if (has_skin) {
                const i32 skin = skin_of_mesh[index];
                if (skin >= 0 && std::cmp_less(skin, rig.slot_to_joint.size())) {
                    slot_map = &rig.slot_to_joint[static_cast<usize>(skin)];
                }
            }
            // Attribute arrays must stay one-to-one with positions, so a primitive that supplies an
            // attribute for some vertices and not others is padded rather than left ragged — which
            // `MeshData::validate` would otherwise refuse for the whole mesh.
            for (usize vertex = 0; vertex < vertex_count; ++vertex) {
                if (normals.size() == vertex_count * 3) {
                    const Vec3 raw{normals[(vertex * 3) + 0], normals[(vertex * 3) + 1],
                                   normals[(vertex * 3) + 2]};
                    if (Status pushed = built.normals.push_back(direction_to_engine(state, raw));
                        !pushed) {
                        return pushed;
                    }
                } else if (!built.normals.empty()) {
                    if (Status pushed = built.normals.push_back(Vec3{0.0f, 1.0f, 0.0f}); !pushed) {
                        return pushed;
                    }
                }
                if (uvs.size() == vertex_count * 2) {
                    if (Status pushed =
                            built.uvs.push_back(Vec2{uvs[(vertex * 2) + 0], uvs[(vertex * 2) + 1]});
                        !pushed) {
                        return pushed;
                    }
                } else if (!built.uvs.empty()) {
                    if (Status pushed = built.uvs.push_back(Vec2{0.0f, 0.0f}); !pushed) {
                        return pushed;
                    }
                }
                if (uv2.size() == vertex_count * 2) {
                    if (Status pushed =
                            built.uv2.push_back(Vec2{uv2[(vertex * 2) + 0], uv2[(vertex * 2) + 1]});
                        !pushed) {
                        return pushed;
                    }
                } else if (!built.uv2.empty()) {
                    if (Status pushed = built.uv2.push_back(Vec2{0.0f, 0.0f}); !pushed) {
                        return pushed;
                    }
                }
                if (has_skin) {
                    SkinInfluence influence;
                    f32 total = 0.0f;
                    for (usize lane = 0; lane < kSkinInfluences; ++lane) {
                        const auto skin_slot =
                            static_cast<usize>(joint_slots[(vertex * kSkinInfluences) + lane]);
                        // Through the slot map, never straight: the file's slot and the cooked
                        // joint index are two numberings and confusing them is a character whose
                        // left arm moves when its right leg does.
                        influence.joints[lane] = slot_map != nullptr && skin_slot < slot_map->size()
                                                     ? (*slot_map)[skin_slot]
                                                     : static_cast<u16>(skin_slot);
                        influence.weights[lane] = joint_weights[(vertex * kSkinInfluences) + lane];
                        total += influence.weights[lane];
                    }
                    // RENORMALISED, because glTF only recommends that the four sum to one and a
                    // quantised WEIGHTS_0 accessor rounds away from it by construction. A vertex
                    // whose weights sum to 0.997 shrinks towards the origin under skinning, which
                    // reads as a seam rather than as a weight problem.
                    if (total > 0.0f) {
                        for (f32& weight : influence.weights) {
                            weight /= total;
                        }
                    } else {
                        // No influence at all binds the vertex rigidly to the joint its first slot
                        // names, which is what a cooked mesh's skinning path can actually draw.
                        influence.weights[0] = 1.0f;
                    }
                    if (Status pushed = built.skin.push_back(influence); !pushed) {
                        return pushed;
                    }
                } else if (!built.skin.empty()) {
                    // A primitive with no binding inside a mesh that has one: bound rigidly to the
                    // first joint rather than left ragged, which `MeshData::validate` would refuse
                    // for the whole mesh. The same padding rule the attributes above follow.
                    SkinInfluence influence;
                    influence.weights[0] = 1.0f;
                    if (Status pushed = built.skin.push_back(influence); !pushed) {
                        return pushed;
                    }
                }
            }

            const JsonRef indices_ref = json.member(primitive, "indices");
            const auto first_index = static_cast<u32>(built.indices.size());
            if (indices_ref != JsonDocument::kNone) {
                Expected<std::vector<u32>, Error> indices = read_accessor_indices(
                    document,
                    json.at(accessors, static_cast<usize>(json.integer_or(indices_ref, -1))));
                if (!indices) {
                    if (Status reported = out.report(ImportSeverity::Error, "unreadable-accessor",
                                                     indices.error().message, mesh_names.back());
                        !reported) {
                        return reported;
                    }
                    continue;
                }
                for (const u32 index_value : indices.value()) {
                    if (Status pushed = built.indices.push_back(base + index_value); !pushed) {
                        return pushed;
                    }
                }
            } else {
                for (usize vertex = 0; vertex < vertex_count; ++vertex) {
                    if (Status pushed = built.indices.push_back(base + static_cast<u32>(vertex));
                        !pushed) {
                        return pushed;
                    }
                }
            }

            // A z-up conversion mirrors nothing — it is a rotation — so winding is preserved and
            // the index order is left alone. A conversion that DID mirror would have to reverse
            // every triangle here, and the absence of that code is a statement rather than an
            // omission.
            MeshSection section;
            section.first_index = first_index;
            section.index_count = static_cast<u32>(built.indices.size()) - first_index;
            section.material =
                static_cast<u32>(json.integer_or(json.member(primitive, "material"), 0));
            if (Status pushed = built.sections.push_back(section); !pushed) {
                return pushed;
            }
            usable = true;
        }

        if (!usable || built.indices.empty()) {
            continue;
        }

        // A glTF mesh may hold primitives that disagree about which attributes they carry, and the
        // engine's mesh cannot: an attribute array is per vertex or it is absent. The primitive
        // that HAS the attribute is the odd one out, so an attribute that did not reach every
        // vertex is dropped and regenerated below rather than left ragged.
        if (!built.normals.empty() && built.normals.size() != built.positions.size()) {
            built.normals.clear();
        }
        if (!built.uvs.empty() && built.uvs.size() != built.positions.size()) {
            built.uvs.clear();
        }
        if (!built.uv2.empty() && built.uv2.size() != built.positions.size()) {
            built.uv2.clear();
        }
        if (!built.skin.empty() && built.skin.size() != built.positions.size()) {
            built.skin.clear();
        }

        // --- 3, 2 and 4: generate what is missing, weld, then optimise. 5: the level-of-detail
        // chain. Both are `model.h`'s, shared with the FBX importer so that one mesh exported in
        // two formats welds to one vertex count and cooks to one set of bytes.
        if (Status finished = finish_mesh(built, state.build, out, mesh_names.back()); !finished) {
            return finished;
        }
        if (Status emitted = emit_mesh_with_lods(built, mesh_names.back(), state.build, out);
            !emitted) {
            return emitted;
        }

        built_meshes.back() = std::move(built);
    }

    // --- 7, continued: emit the skeleton, and 8: the clips.
    //
    // AFTER THE MESHES because the sub-asset names come from `names`, which numbers every
    // sub-asset this import produces, and a skeleton that took its name before the meshes would
    // renumber every one of them — re-minting `AssetId`s that a project has already bound.
    if (!rig.skeleton.joints.empty() && state.import_skins) {
        std::string_view stem;
        for (const ImportedJoint& joint : rig.skeleton.joints) {
            if (joint.parent < 0) {
                stem = strip_joint_namespace(joint.name);
                break;
            }
        }
        rig.skeleton.name = names.unique(kSkeletonSubAssetPrefix, stem, 0);
        Array<u8> skeleton_payload;
        if (Status written = write_cooked_skeleton(rig.skeleton, skeleton_payload); !written) {
            return written;
        }
        if (Status added = out.add(assets::AssetKind::Animation, rig.skeleton.name,
                                   std::move(skeleton_payload), false);
            !added) {
            return added;
        }
        if (Status checked =
                check_inverse_bind(json, root, document, state, rig, out, request.source.view());
            !checked) {
            return checked;
        }
        char detail[ImportDiagnostic::kDetailCapacity] = {};
        (void)std::snprintf(
            detail, sizeof(detail), "%u joint(s), %u of the 22 standard humanoid joints mapped",
            static_cast<u32>(rig.skeleton.joints.size()), rig.skeleton.humanoid.mapped_count());
        if (Status reported = out.report(ImportSeverity::Info, "imported-skeleton", detail,
                                         request.source.view());
            !reported) {
            return reported;
        }
    }

#ifdef CY_IMPORT_ANIMATION
    GltfClipReport clips;
    if (state.import_animations && !rig.too_many_joints) {
        if (Status imported = import_gltf_animations(json, root, document, state, rig,
                                                     request.source.view(), names, out, clips);
            !imported) {
            return imported;
        }
    }
    // "the achieved compression ratio and worst-case error SHALL be reported", and the numbers are
    // MEASURED by the codec against the authored keys rather than estimated from the settings.
    if (clips.clips != 0) {
        char detail[ImportDiagnostic::kDetailCapacity] = {};
        (void)std::snprintf(detail, sizeof(detail),
                            "%u clip(s), %u tracks, %u keys from %u authored; worst error %.3f mm "
                            "and %.3f degrees",
                            clips.clips, clips.tracks, clips.keys_after, clips.keys_before,
                            static_cast<f64>(clips.worst_translation_mm),
                            static_cast<f64>(clips.worst_rotation_degrees));
        if (Status reported = out.report(ImportSeverity::Info, "imported-animation", detail,
                                         request.source.view());
            !reported) {
            return reported;
        }
    }
    if (clips.unmapped_channels != 0) {
        char detail[ImportDiagnostic::kDetailCapacity] = {};
        (void)std::snprintf(detail, sizeof(detail),
                            "%u animation channel(s) target something outside this file's joint "
                            "table, so their motion was not imported",
                            clips.unmapped_channels);
        if (Status reported = out.report(ImportSeverity::Warning, "unmapped-animation", detail,
                                         request.source.view());
            !reported) {
            return reported;
        }
    }
    if (clips.morph_channels != 0) {
        // Morph targets are step 9's neighbour and no step of this importer reads them. Named,
        // because a file whose facial animation vanished without a word is the silent drop this
        // whole section exists to end.
        char detail[ImportDiagnostic::kDetailCapacity] = {};
        (void)std::snprintf(detail, sizeof(detail),
                            "%u animation channel(s) drive morph target weights, which this "
                            "importer does not read; everything else in them came through",
                            clips.morph_channels);
        if (Status reported = out.report(ImportSeverity::Warning, "skipped-morph-targets", detail,
                                         request.source.view());
            !reported) {
            return reported;
        }
    }
#else
    // `-D CY_ANIMATION=OFF` removed src/animation/ from the build, so there is no clip codec to
    // compress with and step 8 was NOT REACHED. `asset-import-pipeline` draws exactly that line: a
    // step skipped for that reason is not a warning about the file and SHALL NOT be reported as
    // one. `info.steps` says the same thing to the derivation key.
    if (state.import_animations && json.size(json.member(root, "animations")) != 0) {
        if (Status reported = out.report(
                ImportSeverity::Info, "animation-runtime-absent",
                "this build was configured with CY_ANIMATION=OFF, so there is no clip codec to "
                "compress with and step 8 was not reached; the animations in this file were read "
                "by nothing",
                request.source.view());
            !reported) {
            return reported;
        }
    }
#endif

    // --- 10. The hierarchy.
    std::vector<ImportedNode> nodes;
    std::vector<std::string> node_names;
    const JsonRef node_array = json.member(root, "nodes");
    const usize node_count = json.size(node_array);
    node_names.reserve(node_count);
    for (usize index = 0; index < node_count; ++index) {
        node_names.emplace_back(
            json.string_or(json.member(json.at(node_array, index), "name"), ""));
    }

    // Depth-first from the scene's roots, so a parent's index is always below its children's —
    // which is what lets a reader build the hierarchy in one forward pass.
    std::vector<i32> stack_nodes;
    std::vector<i32> stack_parents;
    const JsonRef scenes = json.member(root, "scenes");
    const auto scene_index = static_cast<usize>(json.integer_or(json.member(root, "scene"), 0));
    const JsonRef scene = json.at(scenes, scene_index);
    const JsonRef scene_roots = json.member(scene, "nodes");
    for (usize index = json.size(scene_roots); index > 0; --index) {
        stack_nodes.push_back(
            static_cast<i32>(json.integer_or(json.at(scene_roots, index - 1), -1)));
        stack_parents.push_back(-1);
    }
    if (stack_nodes.empty()) {
        // A glTF with no scene still has nodes worth importing; treating every node as a root is
        // what every other importer does and is better than producing an empty prefab.
        for (usize index = node_count; index > 0; --index) {
            stack_nodes.push_back(static_cast<i32>(index - 1));
            stack_parents.push_back(-1);
        }
    }

    /// The glTF mesh each node came from, kept beside `nodes` so a collision node can be built from
    /// the SOURCE mesh after the walk rather than from whatever it was simplified to.
    std::vector<i32> node_source_mesh;
    while (!stack_nodes.empty()) {
        const i32 source_index = stack_nodes.back();
        const i32 parent = stack_parents.back();
        stack_nodes.pop_back();
        stack_parents.pop_back();
        if (source_index < 0 || std::cmp_greater_equal(source_index, node_count)) {
            continue;
        }
        const JsonRef source_node = json.at(node_array, static_cast<usize>(source_index));

        ImportedNode node;
        node.name = node_names[static_cast<usize>(source_index)];
        node.parent = parent;

        const JsonRef matrix = json.member(source_node, "matrix");
        if (json.size(matrix) == 16) {
            // Column-major, per glTF. Decomposed rather than kept as a matrix: the engine's
            // transform is TRS and a node that carried a matrix would be the one node nothing could
            // animate or edit.
            Vec3 columns[3];
            for (usize column = 0; column < 3; ++column) {
                columns[column] =
                    Vec3{static_cast<f32>(json.number_or(json.at(matrix, (column * 4) + 0), 0.0)),
                         static_cast<f32>(json.number_or(json.at(matrix, (column * 4) + 1), 0.0)),
                         static_cast<f32>(json.number_or(json.at(matrix, (column * 4) + 2), 0.0))};
            }
            node.scale = Vec3{length(columns[0]), length(columns[1]), length(columns[2])};
            node.rotation = Quat::from_basis(normalized_or(columns[0], Vec3{1, 0, 0}),
                                             normalized_or(columns[1], Vec3{0, 1, 0}),
                                             normalized_or(columns[2], Vec3{0, 0, 1}));
            node.translation =
                to_engine(state, Vec3{static_cast<f32>(json.number_or(json.at(matrix, 12), 0.0)),
                                      static_cast<f32>(json.number_or(json.at(matrix, 13), 0.0)),
                                      static_cast<f32>(json.number_or(json.at(matrix, 14), 0.0))});
        } else {
            const JsonRef translation = json.member(source_node, "translation");
            node.translation = to_engine(
                state, Vec3{static_cast<f32>(json.number_or(json.at(translation, 0), 0.0)),
                            static_cast<f32>(json.number_or(json.at(translation, 1), 0.0)),
                            static_cast<f32>(json.number_or(json.at(translation, 2), 0.0))});
            const JsonRef rotation = json.member(source_node, "rotation");
            node.rotation = Quat{static_cast<f32>(json.number_or(json.at(rotation, 0), 0.0)),
                                 static_cast<f32>(json.number_or(json.at(rotation, 1), 0.0)),
                                 static_cast<f32>(json.number_or(json.at(rotation, 2), 0.0)),
                                 static_cast<f32>(json.number_or(json.at(rotation, 3), 1.0))};
            const JsonRef node_scale = json.member(source_node, "scale");
            node.scale = Vec3{static_cast<f32>(json.number_or(json.at(node_scale, 0), 1.0)),
                              static_cast<f32>(json.number_or(json.at(node_scale, 1), 1.0)),
                              static_cast<f32>(json.number_or(json.at(node_scale, 2), 1.0))};
        }

        const i64 mesh_index = json.integer_or(json.member(source_node, "mesh"), -1);
        i32 source_mesh = -1;
        if (mesh_index >= 0 && static_cast<usize>(mesh_index) < mesh_names.size()) {
            node.mesh = static_cast<i32>(mesh_index);
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

        const JsonRef children = json.member(source_node, "children");
        for (usize child = json.size(children); child > 0; --child) {
            stack_nodes.push_back(
                static_cast<i32>(json.integer_or(json.at(children, child - 1), -1)));
            stack_parents.push_back(index_of_node);
        }
    }

    // --- 6. Collision, from the source mesh and never from a level of detail. `model.h`'s, shared
    // with the FBX importer.
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
            return make_unexpected(emitted.error());
        }
        if (emitted.value() != 0) {
            node.collision = static_cast<i32>(colliders);
            ++colliders;
        }
    }

    return emit_prefab(Span<const ImportedNode>(nodes.data(), nodes.size()), out);
}

}  // namespace cy::import
