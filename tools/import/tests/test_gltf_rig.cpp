// glTF steps 7 and 8 — skins, skeletons and animation clips — end to end in memory. M11.b task 6.1.
//
// ================================================================================================
// WHAT THIS SUITE EXISTS FOR
// ================================================================================================
//
// Until M11.b this importer REFUSED both steps by name. `gltf.cpp` reported one `skipped-rig`
// warning for any file carrying `skins` or `animations` and dropped the rig, and the FBX path —
// which did import a skeleton and clips from M8.d — parsed every skin cluster and dropped it,
// because `MeshData` had no joint or weight array to put one in. `samples/09b-animated-character`
// records the consequence in its own header: it DERIVES its skin weights from vertex height.
//
// Every case below therefore asserts something that could not have been true before, and each one
// can fail. The two the milestone ledger names — `a skin imports…` and `an animation imports…` —
// are the first two; the rest are the ways the two numberings can be confused, which is the defect
// this code is most likely to have and the one no smoke test would catch: a character whose left
// arm moves when its right leg does is a green import.
//
// THE DOCUMENTS ARE BUILT HERE rather than checked in, for the reason `test_gltf.cpp` gives: a
// fixture is a binary nobody can read in a review, and a document built in the test says what every
// accessor is for.

#include <cy/core/math/scalar.h>
#include <cy/import/clip_record.h>
#include <cy/import/fbx_clip.h>
#include <cy/import/fbx_skeleton.h>
#include <cy/import/gltf.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cy::import;
using cy::f32;
using cy::i32;
using cy::u16;
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

/// How a skinned document is shaped, so one builder serves every case.
struct RigShape {
    /// The skin's `joints` array, as node indices. The document's nodes are 0 = the mesh node,
    /// 1 = `Hips`, 2 = `Spine`, 3 = `LeftHand`. Listing them out of hierarchy order is what makes
    /// the slot map do work.
    std::vector<u32> skin_joints = {1, 2, 3};
    /// Whether to write `inverseBindMatrices`, and whether to write them WRONG.
    bool inverse_bind = true;
    bool inverse_bind_wrong = false;
    /// Whether to write an `animations` array.
    bool animate = true;
    /// The name the single animation declares. Empty leaves `name` off.
    std::string animation_name;
    /// Extra animation channels targeting morph-target weights, which nothing imports.
    bool morph_channel = false;
};

/// A glTF with a four-vertex skinned quad, a three-joint hierarchy and one rotation animation.
///
/// THE FOUR VERTICES CARRY FOUR DIFFERENT BINDINGS, one per joint plus one blend, and each sits at
/// a position no other vertex shares. That is what lets a case assert the binding of a PARTICULAR
/// vertex after welding, cache optimisation and fetch reordering have permuted the buffer — an
/// assertion by index would pass on a mesh whose bindings had all been swapped.
std::string rig_document(const RigShape& shape) {
    std::vector<u8> buffer;
    // Positions: the quad's corners, each identifiable.
    const f32 positions[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    for (const auto& position : positions) {
        append_f32(buffer, position[0]);
        append_f32(buffer, position[1]);
        append_f32(buffer, position[2]);
    }
    // JOINTS_0, unsigned short, VEC4 — SLOTS in the skin's own `joints` array, not joint indices.
    const usize joints_offset = buffer.size();
    const u32 slots[4][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {2, 0, 0, 0}, {1, 2, 0, 0}};
    for (const auto& vertex : slots) {
        for (const u32 slot : vertex) {
            append_u16(buffer, slot);
        }
    }
    // WEIGHTS_0, float, VEC4. The last vertex is a genuine blend; the third deliberately sums to
    // 0.8, which the importer must renormalise rather than leave shrinking under skinning.
    const usize weights_offset = buffer.size();
    const f32 weights[4][4] = {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.8f, 0.0f, 0.0f, 0.0f},
        {0.25f, 0.75f, 0.0f, 0.0f},
    };
    for (const auto& vertex : weights) {
        for (const f32 weight : vertex) {
            append_f32(buffer, weight);
        }
    }
    const usize index_offset = buffer.size();
    for (const u32 index : {0U, 1U, 2U, 2U, 1U, 3U}) {
        append_u16(buffer, index);
    }
    // The inverse bind matrices, column-major. The hierarchy below places `Hips` at the origin,
    // `Spine` one metre above it and `LeftHand` one metre to the side of the spine, so the global
    // bind translations are (0,0,0), (0,1,0) and (1,1,0) and the inverses negate them.
    const usize inverse_offset = buffer.size();
    const f32 bind_translation[3][3] = {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}};
    for (const auto& bind : bind_translation) {
        const f32 fudge = shape.inverse_bind_wrong ? 0.5f : 0.0f;
        const f32 columns[16] = {1,        0,        0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -bind[0] + fudge,
                                 -bind[1], -bind[2], 1};
        for (const f32 value : columns) {
            append_f32(buffer, value);
        }
    }
    // The animation's sampler: three key times and three quaternions rotating `Spine` about X.
    const usize times_offset = buffer.size();
    for (const f32 time : {0.0f, 0.5f, 1.0f}) {
        append_f32(buffer, time);
    }
    const usize rotations_offset = buffer.size();
    const f32 rotations[3][4] = {{0, 0, 0, 1}, {0.3826834f, 0, 0, 0.9238795f}, {0, 0, 0, 1}};
    for (const auto& rotation : rotations) {
        for (const f32 component : rotation) {
            append_f32(buffer, component);
        }
    }

    const auto view = [](usize offset, usize length) {
        return R"({"buffer":0,"byteOffset":)" + std::to_string(offset) + R"(,"byteLength":)" +
               std::to_string(length) + "}";
    };
    std::string document;
    document += R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)";
    document += std::to_string(buffer.size());
    document += R"(,"uri":"data:application/octet-stream;base64,)";
    document += base64(buffer);
    document += R"("}],"bufferViews":[)";
    document += view(0, 48) + ",";                // 0 positions
    document += view(joints_offset, 32) + ",";    // 1 JOINTS_0
    document += view(weights_offset, 64) + ",";   // 2 WEIGHTS_0
    document += view(index_offset, 12) + ",";     // 3 indices
    document += view(inverse_offset, 192) + ",";  // 4 inverse bind matrices
    document += view(times_offset, 12) + ",";     // 5 key times
    document += view(rotations_offset, 48);       // 6 rotations
    document += R"(],"accessors":[)";
    document += R"({"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},)";
    document += R"({"bufferView":1,"componentType":5123,"count":4,"type":"VEC4"},)";
    document += R"({"bufferView":2,"componentType":5126,"count":4,"type":"VEC4"},)";
    document += R"({"bufferView":3,"componentType":5123,"count":6,"type":"SCALAR"},)";
    document += R"({"bufferView":4,"componentType":5126,"count":3,"type":"MAT4"},)";
    document += R"({"bufferView":5,"componentType":5126,"count":3,"type":"SCALAR"},)";
    document += R"({"bufferView":6,"componentType":5126,"count":3,"type":"VEC4"}],)";
    document +=
        R"("meshes":[{"name":"Body","primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,)"
        R"("WEIGHTS_0":2},"indices":3,"mode":4}]}],)";

    document += R"("skins":[{"joints":[)";
    for (usize index = 0; index < shape.skin_joints.size(); ++index) {
        document += index == 0 ? "" : ",";
        document += std::to_string(shape.skin_joints[index]);
    }
    document += "]";
    if (shape.inverse_bind) {
        document += R"(,"inverseBindMatrices":4)";
    }
    document += R"(}],)";

    // Node 0 draws the mesh through the skin; 1 is the root joint, 2 its child, 3 the child's.
    document += R"("nodes":[{"name":"Body","mesh":0,"skin":0},)"
                R"({"name":"mixamorig:Hips","children":[2]},)"
                R"({"name":"mixamorig:Spine","translation":[0,1,0],"children":[3]},)"
                R"({"name":"mixamorig:LeftHand","translation":[1,0,0]}],)";

    if (shape.animate) {
        document += R"("animations":[{)";
        if (!shape.animation_name.empty()) {
            document += R"("name":")" + shape.animation_name + R"(",)";
        }
        document += R"("samplers":[{"input":5,"output":6,"interpolation":"LINEAR"}],)";
        document += R"("channels":[{"sampler":0,"target":{"node":2,"path":"rotation"}})";
        if (shape.morph_channel) {
            document += R"(,{"sampler":0,"target":{"node":0,"path":"weights"}})";
        }
        document += R"(]}],)";
    }
    document += R"("scenes":[{"nodes":[0,1]}],"scene":0})";
    return document;
}

ImportResult import_document(const std::string& text, const ImportOptions* options) {
    GltfImporter importer;
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise("models/Walking.gltf").value();
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

/// The first sub-asset whose name begins with `prefix`. Used for the clip, whose name is derived.
const SubAsset* find_prefixed(const ImportResult& result, std::string_view prefix) {
    for (const SubAsset& produced : result.assets()) {
        if (produced.view().starts_with(prefix)) {
            return &produced;
        }
    }
    return nullptr;
}

bool has_diagnostic(const ImportResult& result, std::string_view code) {
    const cy::Span<const ImportDiagnostic> diagnostics = result.diagnostics();
    return std::ranges::any_of(diagnostics, [code](const ImportDiagnostic& diagnostic) {
        return std::string_view(diagnostic.code) == code;
    });
}

MeshData mesh_of(const SubAsset& produced) {
    MeshData mesh;
    CY_REQUIRE(
        read_cooked_mesh(cy::Span<const u8>(produced.payload.data(), produced.payload.size()), mesh)
            .has_value());
    return mesh;
}

/// The skin binding of the vertex at `position`, found by position rather than by index.
const SkinInfluence* binding_at(const MeshData& mesh, cy::Vec3 position) {
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        if (cy::math::nearly_equal(mesh.positions[index].x, position.x, 1e-4f) &&
            cy::math::nearly_equal(mesh.positions[index].y, position.y, 1e-4f) &&
            cy::math::nearly_equal(mesh.positions[index].z, position.z, 1e-4f)) {
            return &mesh.skin[index];
        }
    }
    return nullptr;
}

}  // namespace

// ================================================================================================
// The two cases the milestone ledger names
// ================================================================================================

CY_TEST_CASE("a skin imports, with its skeleton and its per-vertex joint bindings") {
    const ImportResult result = import_document(rig_document(RigShape{}), nullptr);
    CY_CHECK(!result.has_errors());
    // The refusal this rung removed. It must not come back: a file carrying a skin no longer
    // "carries skins or animations, which this build does not import".
    CY_CHECK(!has_diagnostic(result, "skipped-rig"));

    // --- The skeleton, named after the root joint with its namespace stripped.
    const SubAsset* skeleton_asset = find(result, "skeleton/Hips");
    CY_REQUIRE(skeleton_asset != nullptr);
    CY_CHECK(skeleton_asset->kind == cy::assets::AssetKind::Animation);

    ImportedSkeleton skeleton;
    CY_REQUIRE(read_cooked_skeleton(cy::Span<const u8>(skeleton_asset->payload.data(),
                                                       skeleton_asset->payload.size()),
                                    skeleton)
                   .has_value());
    CY_REQUIRE(skeleton.joints.size() == 3);
    // Parent before child, which `Skeleton::add_joint` refuses a record for not being.
    CY_CHECK(skeleton.joints[0].name == "mixamorig:Hips");
    CY_CHECK(skeleton.joints[0].parent == -1);
    CY_CHECK(skeleton.joints[1].name == "mixamorig:Spine");
    CY_CHECK(skeleton.joints[1].parent == 0);
    CY_CHECK(skeleton.joints[2].name == "mixamorig:LeftHand");
    CY_CHECK(skeleton.joints[2].parent == 1);
    // The humanoid profile is mapped from the same table the FBX path uses, through the same
    // namespace-stripping rule: `mixamorig:Hips` is the standard `Hips`.
    CY_CHECK(skeleton.humanoid.resolve(1) == 0);
    CY_CHECK(skeleton.humanoid.resolve(2) == 1);
    CY_CHECK(skeleton.humanoid.resolve(9) == 2);
    // A rig with no dedicated root node has its root joint playing `Root` as well as `Hips`.
    CY_CHECK(skeleton.humanoid.resolve(0) == 0);

    // --- The mesh, whose bindings are the half of step 7 that lives on the mesh.
    const SubAsset* mesh_asset = find(result, "mesh/Body");
    CY_REQUIRE(mesh_asset != nullptr);
    const MeshData mesh = mesh_of(*mesh_asset);
    CY_REQUIRE(mesh.vertex_count() == 4);
    CY_REQUIRE(mesh.skin.size() == mesh.vertex_count());

    // Vertex (0,0,0) is bound to skin SLOT 0, which is node 1, which is cooked joint 0.
    const SkinInfluence* hips = binding_at(mesh, cy::Vec3{0.0f, 0.0f, 0.0f});
    CY_REQUIRE(hips != nullptr);
    CY_CHECK(hips->joints[0] == 0);
    CY_CHECK(cy::math::nearly_equal(hips->weights[0], 1.0f, 1e-5f));

    // Vertex (0,1,0) named slot 2 with weights summing to 0.8, and must arrive renormalised: a
    // vertex whose weights sum to less than one shrinks towards the origin under skinning.
    const SkinInfluence* hand = binding_at(mesh, cy::Vec3{0.0f, 1.0f, 0.0f});
    CY_REQUIRE(hand != nullptr);
    CY_CHECK(hand->joints[0] == 2);
    CY_CHECK(cy::math::nearly_equal(hand->weights[0], 1.0f, 1e-5f));

    // And the blend: slots 1 and 2 at a quarter and three quarters.
    const SkinInfluence* blended = binding_at(mesh, cy::Vec3{1.0f, 1.0f, 0.0f});
    CY_REQUIRE(blended != nullptr);
    CY_CHECK(blended->joints[0] == 1);
    CY_CHECK(blended->joints[1] == 2);
    CY_CHECK(cy::math::nearly_equal(blended->weights[0], 0.25f, 1e-5f));
    CY_CHECK(cy::math::nearly_equal(blended->weights[1], 0.75f, 1e-5f));
}

#ifdef CY_IMPORT_ANIMATION

CY_TEST_CASE("an animation imports, compressed, against the skeleton's own joint numbering") {
    RigShape shape;
    shape.animation_name = "Walk";
    const ImportResult result = import_document(rig_document(shape), nullptr);
    CY_CHECK(!result.has_errors());
    CY_CHECK(!has_diagnostic(result, "skipped-rig"));

    const SubAsset* clip_asset = find_prefixed(result, "animation/");
    CY_REQUIRE(clip_asset != nullptr);
    CY_CHECK(clip_asset->kind == cy::assets::AssetKind::Animation);

    CookedClip clip;
    CY_REQUIRE(read_cooked_clip(
                   cy::Span<const u8>(clip_asset->payload.data(), clip_asset->payload.size()), clip)
                   .has_value());

    // ONE ANIMATION TAKES THE FILE'S OWN STEM, which is the naming rule `fbx_clip.h` sets: a
    // library export names every clip the same thing and the file name is what the artist chose.
    CY_CHECK(clip.name == "Walking");
    CY_CHECK(cy::math::nearly_equal(clip.duration, 1.0f, 1e-5f));

    // THE JOINT NAMES RIDE WITH THE CLIP, so a loader can rebind by name or refuse. They are the
    // skeleton's, in the skeleton's order, and not the skin's slot order.
    CY_REQUIRE(clip.joint_names.size() == 3);
    CY_CHECK(clip.joint_names[0] == "mixamorig:Hips");
    CY_CHECK(clip.joint_names[1] == "mixamorig:Spine");
    CY_CHECK(clip.joint_names[2] == "mixamorig:LeftHand");

    // The channel targets node 2, `Spine`, which is cooked joint 1 — NOT skin slot 1 and not node
    // index 2. Confusing the three is the defect this whole numbering exists to prevent.
    CY_REQUIRE(clip.tracks.size() == 1);
    CY_CHECK(clip.tracks[0].joint == 1);
    // `TrackKind::Rotation` is 1 and `Interpolation::Spherical` is 3; the record widens both.
    CY_CHECK(clip.tracks[0].kind == 1);
    CY_CHECK(clip.tracks[0].interpolation == 3);
    // The rotation genuinely moves, so the codec must not have collapsed it to one key.
    CY_CHECK(!clip.tracks[0].constant);
    CY_CHECK(clip.tracks[0].key_count >= 2);
    CY_CHECK(clip.keys.size() == clip.tracks[0].key_count);

    // The import reports what it achieved, measured by the codec rather than estimated.
    CY_CHECK(has_diagnostic(result, "imported-animation"));
}

CY_TEST_CASE("a channel targeting no joint produces no clip and is named") {
    // Motion this import DROPS must be counted and reported: the number of unmapped channels must
    // never be non-zero in silence, which is the defect M8.d closed on the FBX side — an
    // animation-only export imported to a prefab with its animation gone and nobody told.
    std::string document = rig_document(RigShape{});
    const std::string retarget = R"("target":{"node":2,"path":"rotation"})";
    const std::string elsewhere = R"("target":{"node":99,"path":"rotation"})";
    CY_REQUIRE(document.find(retarget) != std::string::npos);
    document.replace(document.find(retarget), retarget.size(), elsewhere);

    const ImportResult result = import_document(document, nullptr);
    CY_CHECK(find_prefixed(result, "animation/") == nullptr);
    // Motion this import DROPPED is named. The number must never be non-zero in silence.
    CY_CHECK(has_diagnostic(result, "unmapped-animation"));
}

CY_TEST_CASE("a morph-target channel is named rather than dropped in silence") {
    RigShape shape;
    shape.morph_channel = true;
    const ImportResult result = import_document(rig_document(shape), nullptr);
    CY_CHECK(has_diagnostic(result, "skipped-morph-targets"));
    // And the joint channel beside it still came through.
    CY_CHECK(find_prefixed(result, "animation/") != nullptr);
}

CY_TEST_CASE("import-animations off produces no clip and still produces the skeleton") {
    ImportOptions options;
    const OptionsSchema schema = gltf_options();
    CY_REQUIRE(options.set(schema, "import-animations", OptionValue::of_bool(false)).has_value());
    const ImportResult result = import_document(rig_document(RigShape{}), &options);
    CY_CHECK(find_prefixed(result, "animation/") == nullptr);
    CY_CHECK(find(result, "skeleton/Hips") != nullptr);
}

#endif  // CY_IMPORT_ANIMATION

// ================================================================================================
// The ways the two numberings can be confused
// ================================================================================================

CY_TEST_CASE("a skin whose joints array is not parent-before-child still binds correctly") {
    // glTF fixes NO ordering on a skin's `joints` array and `Skeleton::add_joint` refuses a parent
    // index that is not smaller than the child's, so the cooked record is built by a hierarchy walk
    // and the slot map is what joins the two numberings. This document lists the joints leaf first,
    // which is legal glTF and which an importer that used the slot as the joint index gets exactly
    // backwards — the case that produces a character whose arm moves when its leg does.
    RigShape shape;
    shape.skin_joints = {3, 2, 1};
    const ImportResult result = import_document(rig_document(shape), nullptr);
    CY_CHECK(!result.has_errors());

    const SubAsset* mesh_asset = find(result, "mesh/Body");
    CY_REQUIRE(mesh_asset != nullptr);
    const MeshData mesh = mesh_of(*mesh_asset);
    CY_REQUIRE(mesh.skin.size() == mesh.vertex_count());

    // Slot 0 is now node 3, `LeftHand`, which the hierarchy walk numbered joint 2.
    const SkinInfluence* first = binding_at(mesh, cy::Vec3{0.0f, 0.0f, 0.0f});
    CY_REQUIRE(first != nullptr);
    CY_CHECK(first->joints[0] == 2);
    // Slot 2 is node 1, `Hips`, joint 0.
    const SkinInfluence* third = binding_at(mesh, cy::Vec3{0.0f, 1.0f, 0.0f});
    CY_REQUIRE(third != nullptr);
    CY_CHECK(third->joints[0] == 0);
}

CY_TEST_CASE("inverse bind matrices that disagree with the joint transforms are reported") {
    // glTF makes a node's TRS and its inverse bind matrix two statements of one fact, so on a
    // well-formed file they agree to a rounding error. A file where they do not is a rig this
    // importer will place differently from the file's intent, and preferring either source in
    // silence is how a character arrives posed with no diagnostic anywhere.
    RigShape agreeing;
    CY_CHECK(
        !has_diagnostic(import_document(rig_document(agreeing), nullptr), "bind-pose-disagrees"));

    RigShape disagreeing;
    disagreeing.inverse_bind_wrong = true;
    CY_CHECK(
        has_diagnostic(import_document(rig_document(disagreeing), nullptr), "bind-pose-disagrees"));
}

CY_TEST_CASE("import-skins off imports the same file as a static model") {
    ImportOptions options;
    const OptionsSchema schema = gltf_options();
    CY_REQUIRE(options.set(schema, "import-skins", OptionValue::of_bool(false)).has_value());
    const ImportResult result = import_document(rig_document(RigShape{}), &options);

    CY_CHECK(find(result, "skeleton/Hips") == nullptr);
    const SubAsset* mesh_asset = find(result, "mesh/Body");
    CY_REQUIRE(mesh_asset != nullptr);
    const MeshData mesh = mesh_of(*mesh_asset);
    // No bindings, and the mesh is otherwise the same mesh.
    CY_CHECK(mesh.skin.empty());
    CY_CHECK(mesh.vertex_count() == 4);
}

CY_TEST_CASE("a skinned import is byte-identical when it is run twice") {
    // `asset-import-pipeline` requires byte-identical output for identical input, and the cook
    // cache's content addressing rests on it. Two of the three records this rung added are new
    // writers, and a hash map or a pointer-ordered walk in either of them would break it.
    const std::string document = rig_document(RigShape{});
    const ImportResult first = import_document(document, nullptr);
    const ImportResult second = import_document(document, nullptr);
    CY_REQUIRE(first.assets().size() == second.assets().size());
    for (usize index = 0; index < first.assets().size(); ++index) {
        CY_CHECK(first.assets()[index].view() == second.assets()[index].view());
        CY_REQUIRE(first.assets()[index].payload.size() == second.assets()[index].payload.size());
        CY_CHECK(std::memcmp(first.assets()[index].payload.data(),
                             second.assets()[index].payload.data(),
                             first.assets()[index].payload.size()) == 0);
    }
}

CY_TEST_CASE("the cooked mesh round-trips a skin binding") {
    // The record grew an attribute bit and a version. A reader of version 1 would have accepted a
    // version-2 skinned mesh as a truncated one, which is why the version moved with the bit.
    MeshData mesh;
    CY_REQUIRE(mesh.positions.push_back(cy::Vec3{0.0f, 0.0f, 0.0f}).has_value());
    CY_REQUIRE(mesh.positions.push_back(cy::Vec3{1.0f, 0.0f, 0.0f}).has_value());
    CY_REQUIRE(mesh.positions.push_back(cy::Vec3{0.0f, 1.0f, 0.0f}).has_value());
    for (const u32 index : {0U, 1U, 2U}) {
        CY_REQUIRE(mesh.indices.push_back(index).has_value());
    }
    SkinInfluence influence;
    influence.joints[0] = 7;
    influence.joints[1] = 200;
    influence.weights[0] = 0.75f;
    influence.weights[1] = 0.25f;
    for (usize vertex = 0; vertex < 3; ++vertex) {
        CY_REQUIRE(mesh.skin.push_back(influence).has_value());
    }

    cy::Array<u8> payload;
    CY_REQUIRE(write_cooked_mesh(mesh, payload).has_value());
    MeshData read;
    CY_REQUIRE(
        read_cooked_mesh(cy::Span<const u8>(payload.data(), payload.size()), read).has_value());
    CY_REQUIRE(read.skin.size() == 3);
    CY_CHECK(read.skin[0].joints[0] == 7);
    CY_CHECK(read.skin[0].joints[1] == 200);
    CY_CHECK(cy::math::nearly_equal(read.skin[0].weights[0], 0.75f, 1e-6f));
    CY_CHECK(cy::math::nearly_equal(read.skin[0].weights[1], 0.25f, 1e-6f));
}

CY_TEST_CASE("welding keeps two identically placed vertices with different bindings apart") {
    // A RIG SEAM, and it is the argument a texture seam makes: two vertices in one place bound to
    // different joints are two vertices, and merging them gives one of the two surfaces the
    // other's motion. Without the binding in the weld key this mesh comes back with one vertex.
    MeshData mesh;
    for (usize vertex = 0; vertex < 2; ++vertex) {
        CY_REQUIRE(mesh.positions.push_back(cy::Vec3{0.0f, 0.0f, 0.0f}).has_value());
    }
    SkinInfluence left;
    left.joints[0] = 1;
    left.weights[0] = 1.0f;
    SkinInfluence right;
    right.joints[0] = 2;
    right.weights[0] = 1.0f;
    CY_REQUIRE(mesh.skin.push_back(left).has_value());
    CY_REQUIRE(mesh.skin.push_back(right).has_value());

    WeldOptions options;
    const cy::Expected<usize, cy::Error> removed = weld(mesh, options);
    CY_REQUIRE(removed.has_value());
    CY_CHECK(removed.value() == 0);
    CY_CHECK(mesh.vertex_count() == 2);

    // And the same two vertices with the SAME binding do merge, which is what makes the case above
    // a statement about the binding rather than about welding being off.
    MeshData same;
    for (usize vertex = 0; vertex < 2; ++vertex) {
        CY_REQUIRE(same.positions.push_back(cy::Vec3{0.0f, 0.0f, 0.0f}).has_value());
        CY_REQUIRE(same.skin.push_back(left).has_value());
    }
    const cy::Expected<usize, cy::Error> merged = weld(same, options);
    CY_REQUIRE(merged.has_value());
    CY_CHECK(merged.value() == 1);
    CY_CHECK(same.vertex_count() == 1);
    CY_CHECK(same.skin.size() == 1);
}

// ================================================================================================
// The mesh steps, each of which can lose a binding on its own
// ================================================================================================
//
// `MeshData::skin` is carried by six places in `mesh.cpp` and one in `unwrap.cpp`: the weld's
// gather, the normal split, the fetch reorder, the simplifier's emit, the unwrap's remap and the
// weld key. The import cases above exercise the weld and nothing else — a four-vertex quad's
// indices already reference its vertices in order, so the fetch reorder returns early and a
// mutation that deleted its skin copy left every case above green. These cases drive each step on
// a mesh shaped to make it work.

namespace {

/// A grid of `side + 1` squared vertices, two triangles per cell, each vertex bound to a joint
/// derived from its position so a permutation is visible.
MeshData skinned_grid(u32 side, bool reversed_indices) {
    MeshData mesh;
    for (u32 row = 0; row <= side; ++row) {
        for (u32 column = 0; column <= side; ++column) {
            const f32 x = static_cast<f32>(column);
            const f32 z = static_cast<f32>(row);
            CY_REQUIRE(mesh.positions.push_back(cy::Vec3{x, 0.0f, z}).has_value());
            CY_REQUIRE(
                mesh.uvs.push_back(cy::Vec2{x / static_cast<f32>(side), z / static_cast<f32>(side)})
                    .has_value());
            SkinInfluence influence;
            // A joint per vertex, so "the binding followed its vertex" is checkable per vertex.
            influence.joints[0] = static_cast<u16>((row * (side + 1)) + column);
            influence.weights[0] = 1.0f;
            CY_REQUIRE(mesh.skin.push_back(influence).has_value());
        }
    }
    std::vector<u32> indices;
    for (u32 row = 0; row < side; ++row) {
        for (u32 column = 0; column < side; ++column) {
            const u32 base = (row * (side + 1)) + column;
            for (const u32 corner :
                 {base, base + 1, base + side + 1, base + side + 1, base + 1, base + side + 2}) {
                indices.push_back(corner);
            }
        }
    }
    if (reversed_indices) {
        // The fetch reorder returns early when the index list already references the vertices in
        // order, which a grid's does. Reversing the triangles makes it do its work.
        std::vector<u32> flipped;
        for (usize triangle = indices.size() / 3; triangle > 0; --triangle) {
            for (usize corner = 0; corner < 3; ++corner) {
                flipped.push_back(indices[((triangle - 1) * 3) + corner]);
            }
        }
        indices = flipped;
    }
    for (const u32 index : indices) {
        CY_REQUIRE(mesh.indices.push_back(index).has_value());
    }
    MeshSection section;
    section.first_index = 0;
    section.index_count = static_cast<u32>(mesh.indices.size());
    CY_REQUIRE(mesh.sections.push_back(section).has_value());
    return mesh;
}

/// Every vertex's binding, keyed by its position, so a permutation can be compared across a step.
std::vector<std::pair<cy::Vec3, u16>> bindings_by_position(const MeshData& mesh) {
    std::vector<std::pair<cy::Vec3, u16>> out;
    out.reserve(mesh.vertex_count());
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        out.emplace_back(mesh.positions[index], mesh.skin[index].joints[0]);
    }
    return out;
}

}  // namespace

CY_TEST_CASE("the vertex fetch reorder permutes the bindings with the vertices") {
    MeshData mesh = skinned_grid(4, true);
    const std::vector<std::pair<cy::Vec3, u16>> before = bindings_by_position(mesh);
    CY_REQUIRE(optimise_vertex_fetch(mesh).has_value());
    // It really did reorder: otherwise this case would pass on a step that did nothing.
    CY_REQUIRE(mesh.vertex_count() == before.size());
    bool permuted = false;
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        permuted = permuted || !(mesh.positions[index] == before[index].first);
    }
    CY_CHECK(permuted);

    CY_REQUIRE(mesh.skin.size() == mesh.vertex_count());
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        const SkinInfluence* found = binding_at(mesh, before[index].first);
        CY_REQUIRE(found != nullptr);
        CY_CHECK(found->joints[0] == before[index].second);
    }
}

CY_TEST_CASE("generating normals duplicates a binding along with the vertex it splits") {
    // A hard edge splits a vertex into one per smoothing group, and every other attribute array is
    // duplicated with it. A binding that was not would leave the second half of the split rigidly
    // bound to joint 0 — a crease that moves with the wrong bone.
    MeshData mesh;
    // Two triangles meeting at a right angle along the edge (0,0,0)-(1,0,0).
    for (const cy::Vec3 position :
         {cy::Vec3{0, 0, 0}, cy::Vec3{1, 0, 0}, cy::Vec3{0, 0, 1}, cy::Vec3{0, 1, 0}}) {
        CY_REQUIRE(mesh.positions.push_back(position).has_value());
        SkinInfluence influence;
        influence.joints[0] = static_cast<u16>(mesh.positions.size());
        influence.weights[0] = 1.0f;
        CY_REQUIRE(mesh.skin.push_back(influence).has_value());
    }
    for (const u32 index : {0U, 1U, 2U, 0U, 3U, 1U}) {
        CY_REQUIRE(mesh.indices.push_back(index).has_value());
    }
    MeshSection section;
    section.index_count = 6;
    CY_REQUIRE(mesh.sections.push_back(section).has_value());

    CY_REQUIRE(generate_normals(mesh, 30.0f).has_value());
    // The split happened, which is what makes the assertion below about the binding.
    CY_CHECK(mesh.vertex_count() > 4);
    CY_REQUIRE(mesh.skin.size() == mesh.vertex_count());
    // Every vertex still carries a real binding rather than the default one.
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        CY_CHECK(mesh.skin[index].joints[0] != 0);
        CY_CHECK(cy::math::nearly_equal(mesh.skin[index].weights[0], 1.0f, 1e-6f));
    }
}

CY_TEST_CASE("simplification keeps a binding on every surviving vertex") {
    MeshData mesh = skinned_grid(8, false);
    const usize before = mesh.triangle_count();
    SimplifyOptions options;
    options.target_ratio = 0.5f;
    const cy::Expected<SimplifyReport, cy::Error> report = simplify(mesh, options);
    CY_REQUIRE(report.has_value());
    // It really did simplify, or the case says nothing.
    CY_CHECK(report.value().triangles_after < before);
    CY_REQUIRE(mesh.skin.size() == mesh.vertex_count());
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        // A collapse keeps the surviving vertex's own binding, never an average of two, so every
        // weight is still the 1.0 the grid authored.
        CY_CHECK(cy::math::nearly_equal(mesh.skin[index].weights[0], 1.0f, 1e-6f));
    }
    CY_CHECK(mesh.validate().has_value());
}

CY_TEST_CASE("a lightmap unwrap carries the bindings through the vertices it splits") {
    // A chart boundary is a UV discontinuity, so vertices on it are split and every other
    // attribute is duplicated. A binding left behind is a rig lost at exactly the seams a lightmap
    // put in the mesh.
    MeshData mesh = skinned_grid(6, false);
    CY_REQUIRE(generate_normals(mesh, 60.0f).has_value());
    // `generate_normals` splits, so the grid's per-vertex joints no longer map one to one; what is
    // being asserted is that the count and the weights survive the unwrap.
    Uv2Options options;
    const cy::Expected<Uv2Report, cy::Error> report = generate_uv2(mesh, options);
    CY_REQUIRE(report.has_value());
    CY_CHECK(report.value().charts >= 1);
    CY_REQUIRE(mesh.skin.size() == mesh.vertex_count());
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        CY_CHECK(cy::math::nearly_equal(mesh.skin[index].weights[0], 1.0f, 1e-6f));
    }
    CY_CHECK(mesh.validate().has_value());
}

CY_TEST_CASE("validate refuses a mesh whose binding count is unlike its vertex count") {
    // Every step above calls `validate()` before it starts, because a step that assumed a
    // well-formed mesh and got a malformed one reads out of bounds — and a skin array is the
    // newest way for a mesh to be malformed.
    MeshData mesh = skinned_grid(2, false);
    CY_CHECK(mesh.validate().has_value());
    mesh.skin.pop_back();
    CY_CHECK(!mesh.validate().has_value());
}
