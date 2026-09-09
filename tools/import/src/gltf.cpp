#include <cy/import/gltf.h>

#include <cy/core/assets/hash.h>
#include <cy/core/math/scalar.h>
#include <cy/import/json.h>
#include <cy/import/model.h>

#include <cmath>
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
};

constexpr std::string_view kExtensions[] = {".gltf", ".glb"};
constexpr assets::AssetKind kProduces[] = {assets::AssetKind::Mesh, assets::AssetKind::Material,
                                           assets::AssetKind::Prefab};

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

    if (Status reserved =
            out.reserve(out.size() + 32 + (mesh.vertex_count() * 48) + (mesh.indices.size() * 4));
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
    needed += indices * 4;
    needed += sections * 12;
    if (payload.size() != needed) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked mesh whose payload does not match its header");
    }

    out.clear();
    usize cursor = kHeaderBytes;
    for (usize index = 0; index < vertices; ++index) {
        const Vec3 position{get_f32(payload.data() + cursor), get_f32(payload.data() + cursor + 4),
                            get_f32(payload.data() + cursor + 8)};
        cursor += 12;
        if (Status pushed = out.positions.push_back(position); !pushed) {
            return pushed;
        }
    }
    if (has(attributes, MeshAttributes::Normals)) {
        for (usize index = 0; index < vertices; ++index) {
            const Vec3 normal{get_f32(payload.data() + cursor),
                              get_f32(payload.data() + cursor + 4),
                              get_f32(payload.data() + cursor + 8)};
            cursor += 12;
            if (Status pushed = out.normals.push_back(normal); !pushed) {
                return pushed;
            }
        }
    }
    if (has(attributes, MeshAttributes::TexCoords)) {
        for (usize index = 0; index < vertices; ++index) {
            const Vec2 uv{get_f32(payload.data() + cursor), get_f32(payload.data() + cursor + 4)};
            cursor += 8;
            if (Status pushed = out.uvs.push_back(uv); !pushed) {
                return pushed;
            }
        }
    }
    if (has(attributes, MeshAttributes::TexCoords2)) {
        for (usize index = 0; index < vertices; ++index) {
            const Vec2 uv{get_f32(payload.data() + cursor), get_f32(payload.data() + cursor + 4)};
            cursor += 8;
            if (Status pushed = out.uv2.push_back(uv); !pushed) {
                return pushed;
            }
        }
    }
    if (has(attributes, MeshAttributes::Tangents)) {
        for (usize index = 0; index < vertices; ++index) {
            const Vec4 tangent{
                get_f32(payload.data() + cursor), get_f32(payload.data() + cursor + 4),
                get_f32(payload.data() + cursor + 8), get_f32(payload.data() + cursor + 12)};
            cursor += 16;
            if (Status pushed = out.tangents.push_back(tangent); !pushed) {
                return pushed;
            }
        }
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
    info.version = 2;
    info.extensions = Span<const std::string_view>(kExtensions);
    info.produces = Span<const assets::AssetKind>(kProduces);
    info.description =
        "Imports a glTF 2.0 file — .gltf or .glb — into cooked meshes with levels of detail, "
        "materials, collision proxies from a naming convention, and the node hierarchy the cook "
        "step turns into a prefab.";
    // The eight of the ten model-import steps this build reaches. 7 and 8 — skeletons and
    // animations — are absent for the reason gltf.h states at length, and the report NAMES them
    // rather than warning about them. M8.a task 3.3.
    info.steps = kHierarchyModelSteps;
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
    if (!scale || !up || !meshes || !materials || !weld_tolerance || !smoothing || !tangents ||
        !optimise_option || !overdraw || !lightmap || !density || !padding || !lod_count ||
        !lod_ratio || !lod_error || !collision_suffix || !collision_mode) {
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

    // --- Report what is read and skipped, once, rather than per primitive.
    if (json.size(json.member(root, "skins")) != 0 ||
        json.size(json.member(root, "animations")) != 0) {
        if (Status reported = out.report(
                ImportSeverity::Warning, "skipped-rig",
                "this file carries skins or animations, which this build does not import: "
                "animation-and-skinning reaches Working at M8 and there is nothing to import a "
                "skeleton into before it. Meshes and materials came through.",
                request.source.view());
            !reported) {
            return reported;
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
