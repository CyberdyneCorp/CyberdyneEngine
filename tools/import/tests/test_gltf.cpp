// glTF import, end to end in memory. M5 task 5.1.
//
// The documents are built here rather than checked in as fixtures. A fixture is a binary file
// somebody has to maintain and nobody can read in a review; a document built in the test says what
// every accessor and every node is for, which is what makes a failure diagnosable.

#include <cy/core/math/scalar.h>
#include <cy/import/gltf.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace cy::import;
using cy::f32;
using cy::u32;
using cy::u8;
using cy::usize;

namespace {

std::string base64(const std::vector<u8>& bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    for (usize index = 0; index < bytes.size(); index += 3) {
        const u32 a = bytes[index];
        const u32 b = index + 1 < bytes.size() ? bytes[index + 1] : 0U;
        const u32 c = index + 2 < bytes.size() ? bytes[index + 2] : 0U;
        const u32 packed = (a << 16U) | (b << 8U) | c;
        text += kAlphabet[(packed >> 18U) & 0x3FU];
        text += kAlphabet[(packed >> 12U) & 0x3FU];
        text += index + 1 < bytes.size() ? kAlphabet[(packed >> 6U) & 0x3FU] : '=';
        text += index + 2 < bytes.size() ? kAlphabet[packed & 0x3FU] : '=';
    }
    return text;
}

void append_f32(std::vector<u8>& bytes, f32 value) {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (u32 index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<u8>((bits >> (index * 8U)) & 0xFFU));
    }
}

void append_u16(std::vector<u8>& bytes, u32 value) {
    bytes.push_back(static_cast<u8>(value & 0xFFU));
    bytes.push_back(static_cast<u8>((value >> 8U) & 0xFFU));
}

/// A glTF holding one quad — four vertices, two triangles, positions and texture coordinates —
/// under a node the caller names, with the buffer embedded as a data URI.
///
/// The quad lies in the XY plane so that the z-up conversion has something visible to do: its
/// positions have a non-zero Y that a rotation about X must move onto Z.
std::string quad_document(std::string_view node_name, bool with_second_node) {
    std::vector<u8> buffer;
    // Positions: (0,0,0) (1,0,0) (0,1,0) (1,1,0)
    const f32 positions[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    for (const auto& position : positions) {
        append_f32(buffer, position[0]);
        append_f32(buffer, position[1]);
        append_f32(buffer, position[2]);
    }
    const usize uv_offset = buffer.size();
    const f32 uvs[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (const auto& uv : uvs) {
        append_f32(buffer, uv[0]);
        append_f32(buffer, uv[1]);
    }
    const usize index_offset = buffer.size();
    for (const u32 index : {0U, 1U, 2U, 2U, 1U, 3U}) {
        append_u16(buffer, index);
    }

    std::string document;
    document += R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)";
    document += std::to_string(buffer.size());
    document += R"(,"uri":"data:application/octet-stream;base64,)";
    document += base64(buffer);
    document += R"("}],"bufferViews":[)";
    document += R"({"buffer":0,"byteOffset":0,"byteLength":48},)";
    document +=
        R"({"buffer":0,"byteOffset":)" + std::to_string(uv_offset) + R"(,"byteLength":32},)";
    document +=
        R"({"buffer":0,"byteOffset":)" + std::to_string(index_offset) + R"(,"byteLength":12}],)";
    document += R"("accessors":[)";
    document += R"({"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},)";
    document += R"({"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},)";
    document += R"({"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"}],)";
    document +=
        R"("materials":[{"name":"Oak","pbrMetallicRoughness":{"baseColorFactor":[0.8,0.6,0.4,1.0],)"
        R"("metallicFactor":0.0,"roughnessFactor":0.7}}],)";
    document +=
        R"("meshes":[{"name":"Panel","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},)"
        R"("indices":2,"material":0,"mode":4}]}],)";
    document += R"("nodes":[{"name":")";
    document += node_name;
    document += R"(","mesh":0,"translation":[1,2,3])";
    if (with_second_node) {
        document += R"(,"children":[1]},{"name":"Child","translation":[0,1,0]})";
    } else {
        document += "}";
    }
    document += R"(],"scenes":[{"nodes":[0]}],"scene":0})";
    return document;
}

ImportResult import_document(const std::string& text, const ImportOptions* options) {
    GltfImporter importer;
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise("models/panel.gltf").value();
    request.bytes = cy::Span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size());
    request.options = options;
    ImportResult result;
    CY_REQUIRE(importer.import(request, result).has_value());
    return result;
}

const SubAsset* find(const ImportResult& result, std::string_view name) {
    for (const SubAsset& produced : result.assets()) {
        if (produced.view() == name) {
            return &produced;
        }
    }
    return nullptr;
}

MeshData mesh_of(const SubAsset& produced) {
    MeshData mesh;
    CY_REQUIRE(
        read_cooked_mesh(cy::Span<const u8>(produced.payload.data(), produced.payload.size()), mesh)
            .has_value());
    return mesh;
}

}  // namespace

CY_TEST_CASE("gltf: a document produces a mesh, a material and a prefab") {
    const ImportResult result = import_document(quad_document("Panel", false), nullptr);
    CY_CHECK(!result.has_errors());

    const SubAsset* mesh = find(result, "mesh/Panel");
    CY_REQUIRE(mesh != nullptr);
    CY_CHECK(mesh->kind == cy::assets::AssetKind::Mesh);

    const SubAsset* material = find(result, "material/Oak");
    CY_REQUIRE(material != nullptr);
    CY_CHECK(material->kind == cy::assets::AssetKind::Material);

    const SubAsset* prefab = find(result, "prefab");
    CY_REQUIRE(prefab != nullptr);
    CY_CHECK(prefab->primary);
    CY_CHECK(prefab->kind == cy::assets::AssetKind::Prefab);
    // Exactly one sub-asset is primary: the thing a reference to the FILE resolves to.
    usize primaries = 0;
    for (const SubAsset& produced : result.assets()) {
        primaries += produced.primary ? 1U : 0U;
    }
    CY_CHECK(primaries == 1);
}

CY_TEST_CASE("gltf: the mesh comes through with normals and tangents generated") {
    const ImportResult result = import_document(quad_document("Panel", false), nullptr);
    const SubAsset* produced = find(result, "mesh/Panel");
    CY_REQUIRE(produced != nullptr);
    const MeshData mesh = mesh_of(*produced);
    CY_CHECK(mesh.triangle_count() == 2);
    CY_CHECK(mesh.vertex_count() == 4);
    // The source supplies neither, and every normal-mapped material needs both.
    CY_CHECK(mesh.normals.size() == mesh.vertex_count());
    CY_CHECK(mesh.tangents.size() == mesh.vertex_count());
    CY_CHECK(mesh.uvs.size() == mesh.vertex_count());
}

CY_TEST_CASE("gltf: a z-up source is converted at import") {
    // `asset-import-pipeline`: "WHEN a Z-up model is imported THEN it SHALL be converted at import
    // so no runtime code accounts for source handedness."
    ImportOptions options;
    const OptionsSchema schema = gltf_options();
    CY_REQUIRE(
        options.set(schema, "source-up-axis", OptionValue::of_enumeration("z-up")).has_value());

    const ImportResult converted = import_document(quad_document("Panel", false), &options);
    const SubAsset* produced = find(converted, "mesh/Panel");
    CY_REQUIRE(produced != nullptr);
    const MeshData mesh = mesh_of(*produced);

    // The source's (0,1,0) is up in ITS convention, which is +Z in the engine's — the rotation
    // sends +Z to +Y and +Y to −Z, so a vertex at y = 1 lands at z = −1.
    bool found = false;
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        if (cy::math::nearly_equal(mesh.positions[index].z, -1.0f, 1e-5f)) {
            found = true;
        }
        // Nothing is left at y = 1: the whole quad has been rotated out of the source's plane.
        CY_CHECK(!cy::math::nearly_equal(mesh.positions[index].y, 1.0f, 1e-5f));
    }
    CY_CHECK(found);
}

CY_TEST_CASE("gltf: a scale option is applied at import") {
    ImportOptions options;
    const OptionsSchema schema = gltf_options();
    CY_REQUIRE(options.set(schema, "scale", OptionValue::of_float(0.01)).has_value());

    const ImportResult result = import_document(quad_document("Panel", false), &options);
    const MeshData mesh = mesh_of(*find(result, "mesh/Panel"));
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        CY_CHECK(mesh.positions[index].x <= 0.01f + 1e-6f);
        CY_CHECK(mesh.positions[index].y <= 0.01f + 1e-6f);
    }

    // The node's translation is scaled too: a centimetre model whose nodes were left in centimetres
    // would assemble a hundred times too far apart.
    const SubAsset* prefab = find(result, "prefab");
    CY_REQUIRE(prefab != nullptr);
    cy::Array<ImportedNode> nodes;
    cy::Array<char> names;
    CY_REQUIRE(read_cooked_scene_graph(
                   cy::Span<const u8>(prefab->payload.data(), prefab->payload.size()), nodes, names)
                   .has_value());
    CY_REQUIRE(nodes.size() == 1);
    CY_CHECK(cy::math::nearly_equal(nodes[0].translation.x, 0.01f, 1e-6f));
    CY_CHECK(cy::math::nearly_equal(nodes[0].translation.z, 0.03f, 1e-6f));
}

CY_TEST_CASE("gltf: the hierarchy comes out parent-before-child") {
    const ImportResult result = import_document(quad_document("Root", true), nullptr);
    const SubAsset* prefab = find(result, "prefab");
    CY_REQUIRE(prefab != nullptr);
    cy::Array<ImportedNode> nodes;
    cy::Array<char> names;
    CY_REQUIRE(read_cooked_scene_graph(
                   cy::Span<const u8>(prefab->payload.data(), prefab->payload.size()), nodes, names)
                   .has_value());
    CY_REQUIRE(nodes.size() == 2);
    CY_CHECK(nodes[0].name == "Root");
    CY_CHECK(nodes[0].parent == -1);
    CY_CHECK(nodes[1].name == "Child");
    // A parent's index is always below its children's, which is what lets a reader build the
    // hierarchy in one forward pass.
    CY_CHECK(nodes[1].parent == 0);
}

CY_TEST_CASE("gltf: a node named with the collision suffix becomes a collider and is not drawn") {
    // "WHEN a node is named with the configured collision suffix THEN a collider SHALL be generated
    // from it and the node excluded from rendering."
    const ImportResult result = import_document(quad_document("Panel_collision", false), nullptr);
    const SubAsset* collider = find(result, "collision/Panel_collision");
    CY_REQUIRE(collider != nullptr);

    const SubAsset* prefab = find(result, "prefab");
    cy::Array<ImportedNode> nodes;
    cy::Array<char> names;
    CY_REQUIRE(read_cooked_scene_graph(
                   cy::Span<const u8>(prefab->payload.data(), prefab->payload.size()), nodes, names)
                   .has_value());
    CY_REQUIRE(nodes.size() == 1);
    CY_CHECK(nodes[0].collision_only);
    CY_CHECK(nodes[0].mesh == -1);
    CY_CHECK(nodes[0].collision == 0);

    // A quad is flat, so a convex hull cannot be built from it and the importer falls back to the
    // triangles — saying so rather than producing nothing.
    const MeshData shape = mesh_of(*collider);
    CY_CHECK(shape.triangle_count() >= 2);
    CY_CHECK(shape.normals.empty());
}

CY_TEST_CASE("gltf: the collision convention can be turned off") {
    ImportOptions options;
    const OptionsSchema schema = gltf_options();
    CY_REQUIRE(options.set(schema, "collision-suffix", OptionValue::of_text("")).has_value());
    const ImportResult result = import_document(quad_document("Panel_collision", false), &options);
    CY_CHECK(find(result, "collision/Panel_collision") == nullptr);
}

CY_TEST_CASE("gltf: a level-of-detail chain is generated to the configured depth") {
    ImportOptions options;
    const OptionsSchema schema = gltf_options();
    CY_REQUIRE(options.set(schema, "lod-count", OptionValue::of_int(2)).has_value());
    const ImportResult result = import_document(quad_document("Panel", false), &options);
    CY_CHECK(find(result, "mesh/Panel/lod1") != nullptr);
    CY_CHECK(find(result, "mesh/Panel/lod2") != nullptr);
    CY_CHECK(find(result, "mesh/Panel/lod3") == nullptr);
}

CY_TEST_CASE("gltf: importing twice produces byte-identical output") {
    // "WHEN the same source and options are imported twice THEN the cooked output SHALL be
    // byte-identical." Without it the cook cache's content addressing is worthless.
    const std::string document = quad_document("Panel", true);
    const ImportResult first = import_document(document, nullptr);
    const ImportResult second = import_document(document, nullptr);
    CY_REQUIRE(first.assets().size() == second.assets().size());
    for (usize index = 0; index < first.assets().size(); ++index) {
        CY_REQUIRE(first.assets()[index].view() == second.assets()[index].view());
        CY_REQUIRE(first.assets()[index].payload.size() == second.assets()[index].payload.size());
        for (usize byte = 0; byte < first.assets()[index].payload.size(); ++byte) {
            CY_CHECK(first.assets()[index].payload[byte] == second.assets()[index].payload[byte]);
        }
    }
}

CY_TEST_CASE("gltf: turning meshes off produces no meshes and still produces the hierarchy") {
    // The fast path the specification names for animation iteration, exercised for the half that
    // exists: meshes off still yields the node graph.
    ImportOptions options;
    const OptionsSchema schema = gltf_options();
    CY_REQUIRE(options.set(schema, "import-meshes", OptionValue::of_bool(false)).has_value());
    const ImportResult result = import_document(quad_document("Panel", false), &options);
    CY_CHECK(find(result, "mesh/Panel") == nullptr);
    CY_CHECK(find(result, "prefab") != nullptr);
}

CY_TEST_CASE("gltf: a malformed document is an error on the asset rather than a crash") {
    GltfImporter importer;
    const std::string broken = R"({"asset":{"version":"2.0"},"meshes":[)";
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise("models/broken.gltf").value();
    request.bytes = cy::Span<const u8>(reinterpret_cast<const u8*>(broken.data()), broken.size());
    ImportResult result;
    CY_REQUIRE(importer.import(request, result).has_value());
    CY_CHECK(result.has_errors());
}

CY_TEST_CASE("gltf: a GLB container is read, and one from another version is refused") {
    // The JSON chunk of a GLB is the same document; only the wrapper differs.
    const std::string json = quad_document("Panel", false);
    std::vector<u8> glb;
    const auto append_u32 = [&glb](u32 value) {
        for (u32 index = 0; index < 4; ++index) {
            glb.push_back(static_cast<u8>((value >> (index * 8U)) & 0xFFU));
        }
    };
    const usize padded = (json.size() + 3) & ~usize{3};
    glb.push_back('g');
    glb.push_back('l');
    glb.push_back('T');
    glb.push_back('F');
    append_u32(2);
    append_u32(static_cast<u32>(12 + 8 + padded));
    append_u32(static_cast<u32>(padded));
    append_u32(0x4E4F534AU);
    glb.insert(glb.end(), json.begin(), json.end());
    while (glb.size() < 20 + padded) {
        glb.push_back(' ');
    }

    GltfImporter importer;
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise("models/panel.glb").value();
    request.bytes = cy::Span<const u8>(glb.data(), glb.size());
    ImportResult result;
    CY_REQUIRE(importer.import(request, result).has_value());
    CY_CHECK(!result.has_errors());
    CY_CHECK(find(result, "mesh/Panel") != nullptr);

    glb[4] = 3;  // a container version this build does not read
    ImportResult refused;
    request.bytes = cy::Span<const u8>(glb.data(), glb.size());
    CY_REQUIRE(importer.import(request, refused).has_value());
    CY_CHECK(refused.has_errors());
}

CY_TEST_CASE("gltf: the scene graph's cooked form round-trips") {
    cy::Array<ImportedNode> written;
    ImportedNode root;
    root.name = "Root";
    root.translation = cy::Vec3{1, 2, 3};
    root.mesh = 4;
    CY_REQUIRE(written.push_back(root).has_value());
    ImportedNode child;
    child.name = "Child";
    child.parent = 0;
    child.collision = 7;
    child.collision_only = true;
    CY_REQUIRE(written.push_back(child).has_value());

    cy::Array<u8> payload;
    CY_REQUIRE(write_cooked_scene_graph(
                   cy::Span<const ImportedNode>(written.data(), written.size()), payload)
                   .has_value());

    cy::Array<ImportedNode> read;
    cy::Array<char> names;
    CY_REQUIRE(
        read_cooked_scene_graph(cy::Span<const u8>(payload.data(), payload.size()), read, names)
            .has_value());
    CY_REQUIRE(read.size() == 2);
    CY_CHECK(read[0].name == "Root");
    CY_CHECK(read[0].mesh == 4);
    CY_CHECK(read[1].parent == 0);
    CY_CHECK(read[1].collision == 7);
    CY_CHECK(read[1].collision_only);
}
