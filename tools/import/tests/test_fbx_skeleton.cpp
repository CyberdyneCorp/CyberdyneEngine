// Step 7 of an FBX import: the skeleton, from the file to `cy::animation::Skeleton`. M8.d.
//
// WHAT IS ASSERTED HERE, AND WHY EACH CASE IS NOT A TAUTOLOGY.
//
// The invariants `cy/animation/skeleton.h` enforces are REFUSALS — `add_joint` refuses a forward
// parent reference, `finalize()` refuses a child that outlives its parent's bone level and a
// singular bind pose. A skeleton that loads is therefore a skeleton that satisfied all of them, so
// the round-trip cases below are not "the loop ran": they are the runtime's own checks, run against
// what the importer actually produced from a real file.
//
// THE FIXTURES ARE ASCII FBX BUILT IN C++, for the reason `test_fbx.cpp` gives: a binary fixture is
// a file somebody has to maintain and nobody can read in a review. `Model: … "LimbNode"` with a
// `NodeAttribute … "LimbNode"` connected to it is what an exporter writes for a bone and what ufbx
// reports as `ufbx_node::bone`, which is verified by these cases finding joints at all.
//
// AND ONE CASE READS A REAL MIXAMO EXPORT, which is a 16.7 MB file that cannot live in this
// repository. It runs when `CY_IMPORT_FBX_SAMPLES` names a directory holding `Walking.fbx` and says
// so, loudly, when it does not — a case that quietly passes on a missing input is a case that
// proves nothing. It is the only thing here that exercises a 65-joint rig, a skin whose clusters
// name the bones, a namespaced joint vocabulary and a real bind pose.

#include <cy/animation/skeleton.h>
#include <cy/core/math/scalar.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/animation_bridge.h>
#include <cy/import/fbx.h>
#include <cy/import/fbx_skeleton.h>
#include <cy/import/gltf.h>
#include <cy/test/test.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

using namespace cy::import;
using cy::f32;
using cy::i32;
using cy::u16;
using cy::u32;
using cy::u8;
using cy::usize;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Assets);
}

/// One bone of the ASCII document: a `LimbNode` model, its skeleton attribute, and the two
/// connections that attach it to its parent.
struct Bone {
    std::string_view name;
    int parent;  // an index into the caller's list, or -1 for a child of the scene root
    double x;
    double y;
    double z;
};

/// An ASCII FBX 7.4 carrying nothing but a bone hierarchy.
///
/// `UnitScaleFactor` is 1, which declares a CENTIMETRE file — the unit every Mixamo export and most
/// FBX pipelines write. Every translation below is therefore in centimetres, and an importer that
/// forgot the unit conversion produces a rig a hundred times too large, which is what the metre
/// assertions in these cases catch.
std::string rig_document(const std::vector<Bone>& bones) {
    std::string text =
        "; FBX 7.4.0 project file\n"
        "FBXHeaderExtension:  {\n"
        "\tFBXHeaderVersion: 1003\n"
        "\tFBXVersion: 7400\n"
        "}\n"
        "GlobalSettings:  {\n"
        "\tVersion: 1000\n"
        "\tProperties70:  {\n"
        "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
        "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
        "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
        "\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n"
        "\t}\n"
        "}\n"
        "Objects:  {\n";
    for (usize index = 0; index < bones.size(); ++index) {
        const std::string id = std::to_string(2000 + index);
        const std::string attribute = std::to_string(3000 + index);
        text += "\tNodeAttribute: " + attribute + ", \"NodeAttribute::";
        text += bones[index].name;
        text +=
            "\", \"LimbNode\" {\n"
            "\t\tTypeFlags: \"Skeleton\"\n"
            "\t}\n";
        text += "\tModel: " + id + ", \"Model::";
        text += bones[index].name;
        text +=
            "\", \"LimbNode\" {\n"
            "\t\tVersion: 232\n"
            "\t\tProperties70:  {\n";
        text += "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\"," +
                std::to_string(bones[index].x) + "," + std::to_string(bones[index].y) + "," +
                std::to_string(bones[index].z) + "\n";
        text +=
            "\t\t}\n"
            "\t}\n";
    }
    text += "}\n";
    text += "Connections:  {\n";
    for (usize index = 0; index < bones.size(); ++index) {
        const std::string id = std::to_string(2000 + index);
        const std::string attribute = std::to_string(3000 + index);
        const std::string parent =
            bones[index].parent < 0 ? "0" : std::to_string(2000 + bones[index].parent);
        text += "\tC: \"OO\",";
        text += id;
        text += ",";
        text += parent;
        text += "\n\tC: \"OO\",";
        text += attribute;
        text += ",";
        text += id;
        text += "\n";
    }
    text += "}\n";
    return text;
}

/// The five bones the skinned fixture rigs to. Five rather than four because one of its vertices
/// names FIVE influences, which is the case `kSkinInfluences` exists to bound.
std::vector<Bone> skin_bones() {
    return {
        {"rig:Hips", -1, 0.0, 100.0, 0.0},    {"rig:Spine", 0, 0.0, 20.0, 0.0},
        {"rig:LeftHand", 1, 30.0, 10.0, 0.0}, {"rig:LeftHandIndex1", 2, 5.0, 0.0, 0.0},
        {"rig:Head", 1, 0.0, 30.0, 0.0},
    };
}

/// One cluster of the skinned fixture: which bone it deforms through, and which vertices it moves.
struct Cluster {
    /// An index into `skin_bones()`.
    usize bone;
    std::vector<usize> vertices;
    std::vector<double> weights;
};

/// The clusters, DELIBERATELY IN AN ORDER THE SKELETON DOES NOT SHARE.
///
/// The skeleton record numbers joints by a hierarchy walk — Hips 0, Spine 1, LeftHand 2,
/// LeftHandIndex1 3, Head 4 — and this file lists its clusters leaf first. An importer that used a
/// cluster's own index as the joint index therefore gets every binding wrong, which is the defect
/// this ordering exists to catch: on a fixture whose two orders agreed it would pass.
std::vector<Cluster> skin_clusters() {
    return {
        {3, {2, 3}, {1.0, 0.15}},  // LeftHandIndex1 -> joint 3
        {0, {0, 3}, {1.0, 0.3}},   // Hips           -> joint 0
        {1, {1, 3}, {0.5, 0.25}},  // Spine          -> joint 1
        {2, {1, 3}, {0.5, 0.2}},   // LeftHand       -> joint 2
        {4, {3}, {0.1}},           // Head           -> joint 4
    };
}

/// A bone hierarchy with a FOUR-VERTEX, TWO-POLYGON mesh skinned to it. M11.b task 6.1.
///
/// WHAT THIS EXISTS FOR. `fbx.cpp` read `skin_deformers` from the day ufbx was integrated and had
/// nowhere to put them: `MeshData` carried no joint or weight array, so every cluster was parsed
/// and dropped — 65 of them in `Walking.fbx` — and the only trace was a `skipped-rig` warning.
/// M11.b gave the mesh those arrays. `Walking.fbx` is optional (it lives outside the repository and
/// the cases that use it skip when it is absent), so the FBX skin path needs a fixture that is
/// always there, and this is it.
///
/// TWO POLYGONS AND NOT ONE, which is not decoration. FBX stores attributes per CORNER and skin
/// weights per VERTEX, so a binding is looked up through `vertex_indices`. On a single triangle the
/// corner index and the vertex index are the same three numbers and an importer that confused them
/// would pass; a quad's second triangle re-uses two of the first's vertices, so they differ.
///
/// The four vertices are the four cases: rigid to the first cluster's bone, a half-and-half blend,
/// rigid to a bone whose cluster index is not its joint index, and one named by all FIVE clusters.
std::string skinned_document() {
    std::string text = rig_document(skin_bones());
    const std::vector<Cluster> clusters = skin_clusters();

    std::string objects =
        "\tGeometry: 4000, \"Geometry::Body\", \"Mesh\" {\n"
        "\t\tVertices: *12 {\n\t\t\ta: 0,0,0,100,0,0,0,100,0,100,100,0\n\t\t}\n"
        "\t\tPolygonVertexIndex: *6 {\n\t\t\ta: 0,1,-3,2,1,-4\n\t\t}\n"
        "\t\tGeometryVersion: 124\n"
        "\t}\n"
        "\tModel: 4100, \"Model::Body\", \"Mesh\" {\n\t\tVersion: 232\n\t}\n"
        "\tDeformer: 5000, \"Deformer::Skin\", \"Skin\" {\n"
        "\t\tVersion: 101\n\t\tLink_DeformAcuracy: 50\n\t}\n";
    std::string connections =
        "\tC: \"OO\",4100,0\n"
        "\tC: \"OO\",4000,4100\n"
        "\tC: \"OO\",5000,4000\n";
    for (usize index = 0; index < clusters.size(); ++index) {
        const std::string id = std::to_string(5001 + index);
        objects += "\tDeformer: " + id + ", \"SubDeformer::Cluster" + std::to_string(index) +
                   "\", \"Cluster\" {\n"
                   "\t\tVersion: 100\n\t\tUserData: \"\", \"\"\n";
        objects +=
            "\t\tIndexes: *" + std::to_string(clusters[index].vertices.size()) + " {\n\t\t\ta: ";
        for (usize slot = 0; slot < clusters[index].vertices.size(); ++slot) {
            objects += (slot == 0 ? "" : ",") + std::to_string(clusters[index].vertices[slot]);
        }
        objects += "\n\t\t}\n";
        objects +=
            "\t\tWeights: *" + std::to_string(clusters[index].weights.size()) + " {\n\t\t\ta: ";
        for (usize slot = 0; slot < clusters[index].weights.size(); ++slot) {
            objects += (slot == 0 ? "" : ",") + std::to_string(clusters[index].weights[slot]);
        }
        objects += "\n\t\t}\n";
        objects +=
            "\t\tTransform: *16 {\n\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1\n\t\t}\n"
            "\t\tTransformLink: *16 {\n\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1\n\t\t}\n"
            "\t}\n";
        connections += "\tC: \"OO\"," + id + ",5000\n";
        connections +=
            "\tC: \"OO\"," + std::to_string(2000 + clusters[index].bone) + "," + id + "\n";
    }
    objects += "}\n";
    connections += "}\n";
    // The connection block ends the document and the object block ends just before it. Replacing
    // from the end means a change to the bone writer above does not move either splice point.
    const usize connections_end = text.rfind("}\n");
    text.replace(connections_end, 2, connections);
    const usize objects_end = text.rfind("}\n", connections_end - 1);
    text.replace(objects_end, 2, objects);
    return text;
}

ImportResult import_bytes(cy::Span<const u8> bytes, std::string_view source,
                          const ImportOptions* options) {
    FbxImporter importer;
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise(source).value();
    request.bytes = bytes;
    request.options = options;
    ImportResult result;
    CY_REQUIRE(importer.import(request, result).has_value());
    return result;
}

ImportResult import_document(const std::string& text) {
    return import_bytes(cy::Span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()),
                        "models/rig.fbx", nullptr);
}

const SubAsset* find_skeleton(const ImportResult& result) {
    for (const SubAsset& produced : result.assets()) {
        if (produced.view().starts_with(kSkeletonSubAssetPrefix)) {
            return &produced;
        }
    }
    return nullptr;
}

ImportedSkeleton skeleton_of(const SubAsset& produced) {
    ImportedSkeleton skeleton;
    CY_REQUIRE(read_cooked_skeleton(
                   cy::Span<const u8>(produced.payload.data(), produced.payload.size()), skeleton)
                   .has_value());
    return skeleton;
}

/// The four-bone rig every self-contained case is written against: a root, a spine, a hand and one
/// finger of that hand. The finger is what makes the bone level of detail observable — "a level of
/// detail that drops nothing is not one".
std::vector<Bone> sample_bones() {
    return {
        {"rig:Hips", -1, 0.0, 100.0, 0.0},
        {"rig:Spine", 0, 0.0, 20.0, 0.0},
        {"rig:LeftHand", 1, 30.0, 10.0, 0.0},
        {"rig:LeftHandIndex1", 2, 5.0, 0.0, 0.0},
    };
}

/// Whether `Walking.fbx` is available, and its bytes if so.
///
/// The sample lives outside the repository — it is a 16.7 MB Mixamo export — so the suite is told
/// where to find it rather than guessing, and reports rather than pretends when it is not there.
bool read_sample(const char* file_name, std::vector<u8>& out) {
    const char* directory = std::getenv("CY_IMPORT_FBX_SAMPLES");
    if (directory == nullptr || *directory == '\0') {
        return false;
    }
    std::string path = directory;
    if (!path.empty() && path.back() != '/') {
        path += '/';
    }
    path += file_name;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamoff size = file.tellg();
    file.seekg(0);
    out.resize(static_cast<usize>(size));
    return static_cast<bool>(file.read(reinterpret_cast<char*>(out.data()), size));
}

}  // namespace

// --- The vocabulary the record and the runtime share ---------------------------------------------

CY_TEST_CASE("fbx skeleton: the importer's humanoid vocabulary is the animation runtime's own") {
    // `fbx_skeleton.h` duplicates `HumanoidJoint`'s names because tools/ may not link the animation
    // runtime. THIS is what keeps the duplicate honest: insert a joint into that enum and the
    // spellings slide by one, and every slot after the insertion point would silently address the
    // wrong part of the body.
    CY_REQUIRE_EQ(static_cast<u32>(kHumanoidJointCount),
                  static_cast<u32>(cy::animation::HumanoidJoint::Count));
    for (u16 standard = 0; standard < kHumanoidJointCount; ++standard) {
        const std::string_view runtime =
            cy::animation::humanoid_joint_name(static_cast<cy::animation::HumanoidJoint>(standard));
        CY_CHECK(kHumanoidJointNames[standard] == runtime);
    }
}

CY_TEST_CASE("fbx skeleton: a joint name maps on the part after its namespace, not on the prefix") {
    // Mixamo namespaces every bone, and the namespace carries a digit the moment a rig has been
    // re-exported: all four sample files say `mixamorig1:`, not `mixamorig:`. A table keyed on the
    // literal prefix matches nothing at all on exactly the files this step was written for.
    CY_CHECK_EQ(humanoid_joint_of("mixamorig:Hips"), humanoid_joint_of("mixamorig1:Hips"));
    CY_CHECK_EQ(humanoid_joint_of("mixamorig1:Hips"), u16{1});
    CY_CHECK_EQ(humanoid_joint_of("mixamorig1:LeftForeArm"), u16{8});
    CY_CHECK_EQ(humanoid_joint_of("studio:rigs:RightToeBase"), u16{21});
    // The engine's own spelling maps too, and nothing else does.
    CY_CHECK_EQ(humanoid_joint_of("LeftLowerArm"), u16{8});
    CY_CHECK_EQ(humanoid_joint_of("mixamorig1:LeftHandIndex1"), kUnmappedHumanoidJoint);
    CY_CHECK_EQ(humanoid_joint_of("Bip01 L UpperArm"), kUnmappedHumanoidJoint);
}

// --- The self-contained rig ----------------------------------------------------------------------

CY_TEST_CASE("fbx skeleton: a bone hierarchy imports parent before child, in metres") {
    const ImportResult result = import_document(rig_document(sample_bones()));
    const SubAsset* produced = find_skeleton(result);
    CY_REQUIRE(produced != nullptr);
    // Named from the root joint with its namespace stripped, because the namespace is the half an
    // exporter rewrites and the name is what an `AssetId` is bound to across re-imports.
    CY_CHECK(produced->view() == "skeleton/Hips");
    CY_CHECK(produced->kind == cy::assets::AssetKind::Animation);
    CY_CHECK_FALSE(produced->primary);

    const ImportedSkeleton skeleton = skeleton_of(*produced);
    CY_REQUIRE_EQ(skeleton.joints.size(), usize{4});
    for (usize index = 0; index < skeleton.joints.size(); ++index) {
        CY_CHECK_LT(skeleton.joints[index].parent, static_cast<i32>(index));
    }
    CY_CHECK_EQ(skeleton.joints[0].parent, -1);
    CY_CHECK(skeleton.joints[0].name == "rig:Hips");
    CY_CHECK_EQ(skeleton.joints[1].parent, 0);
    CY_CHECK_EQ(skeleton.joints[3].parent, 2);

    // 100 cm of declared file unit is one metre of engine world unit. `CompressionSettings`
    // converts its translation tolerance with a hard x0.001, so the engine's unit IS the metre and
    // an unconverted rig makes every animation tolerance a hundred times too tight.
    CY_CHECK_NEAR(skeleton.joints[0].bind_local.translation.y, 1.0f, 1e-5);
    CY_CHECK_NEAR(skeleton.joints[1].bind_local.translation.y, 0.2f, 1e-5);
    CY_CHECK_NEAR(skeleton.joints[2].bind_local.translation.x, 0.3f, 1e-5);
}

CY_TEST_CASE("fbx skeleton: the humanoid profile addresses joints the source named differently") {
    const ImportResult result = import_document(rig_document(sample_bones()));
    const ImportedSkeleton skeleton = skeleton_of(*find_skeleton(result));

    CY_CHECK_EQ(skeleton.humanoid.resolve(1), 0);  // Hips
    CY_CHECK_EQ(skeleton.humanoid.resolve(2), 1);  // Spine
    CY_CHECK_EQ(skeleton.humanoid.resolve(9), 2);  // LeftHand
    // The root slot falls to the skeleton's own root, because this rig has no dedicated root node —
    // and a profile with no root is a retarget whose height scale silently stays at 1.
    CY_CHECK_EQ(skeleton.humanoid.resolve(0), 0);
    // A finger plays no standard part, and nothing was invented for it.
    CY_CHECK_EQ(skeleton.humanoid.resolve(4), -1);  // Neck
    CY_CHECK_EQ(skeleton.humanoid.mapped_count(), u32{4});
}

CY_TEST_CASE("fbx skeleton: a finger leaves at bone level 1 and its hand does not") {
    const ImportResult result = import_document(rig_document(sample_bones()));
    const ImportedSkeleton skeleton = skeleton_of(*find_skeleton(result));

    CY_CHECK_EQ(u32{skeleton.joints[2].dropped_at}, u32{kBoneLodLevels});  // the hand survives
    CY_CHECK_EQ(u32{skeleton.joints[3].dropped_at}, u32{1});               // the finger does not
    CY_CHECK_EQ(u32{skeleton.joints[0].dropped_at}, u32{kBoneLodLevels});
}

CY_TEST_CASE(
    "fbx skeleton: the record loads into cy::animation::Skeleton and its bind pose holds") {
    const ImportResult result = import_document(rig_document(sample_bones()));
    const ImportedSkeleton record = skeleton_of(*find_skeleton(result));

    cy::animation::Skeleton skeleton(allocator());
    cy::animation::SkeletonProfile profile;
    // Every refusal the runtime can make — a forward parent reference, a bone level that is not a
    // nested subset, a singular bind pose — is made inside this call.
    CY_REQUIRE(build_runtime_skeleton(record, skeleton, profile).has_value());
    CY_REQUIRE(skeleton.finalized());
    CY_REQUIRE_EQ(skeleton.joint_count(), u16{4});

    // The local-to-model path: the reference pose resolved forward must reproduce what finalize()
    // computed, and the chain must add up in metres. Hips 1.0 + spine 0.2 + hand 0.1.
    cy::Array<cy::Transform> local(allocator());
    cy::Array<cy::Transform> model(allocator());
    CY_REQUIRE(local.resize(4).has_value());
    CY_REQUIRE(model.resize(4).has_value());
    skeleton.reference_pose(local.span());
    skeleton.to_model(local.span(), skeleton.retained(0), model.span());
    for (u16 joint = 0; joint < 4; ++joint) {
        CY_CHECK_NEAR(model[joint].translation.x, skeleton.bind_model()[joint].translation.x, 1e-5);
        CY_CHECK_NEAR(model[joint].translation.y, skeleton.bind_model()[joint].translation.y, 1e-5);
        CY_CHECK_NEAR(model[joint].translation.z, skeleton.bind_model()[joint].translation.z, 1e-5);
    }
    CY_CHECK_NEAR(model[0].translation.y, 1.0f, 1e-5);
    CY_CHECK_NEAR(model[1].translation.y, 1.2f, 1e-5);
    CY_CHECK_NEAR(model[2].translation.y, 1.3f, 1e-5);
    CY_CHECK_NEAR(model[2].translation.x, 0.3f, 1e-5);

    // The profile the record carried reaches the runtime's own side table.
    CY_CHECK_EQ(profile.resolve(cy::animation::HumanoidJoint::LeftHand), u16{2});
    CY_CHECK_EQ(profile.resolve(cy::animation::HumanoidJoint::Hips), u16{0});
    CY_CHECK_EQ(profile.resolve(cy::animation::HumanoidJoint::Neck), cy::animation::kInvalidJoint);

    // The bone level of detail reaches it too: level 0 is every joint and level 1 has lost the
    // finger, which is the mask `evaluate()` intersects an instruction's requirement with.
    CY_CHECK_EQ(skeleton.retained_count(0), u32{4});
    CY_CHECK_EQ(skeleton.retained_count(1), u32{3});
    CY_CHECK_FALSE(skeleton.retained(1).test(3));
}

CY_TEST_CASE("fbx skeleton: two imports of one rig produce byte-identical skeletons") {
    // The determinism requirement, on the bytes rather than on a hash of them, because the cook
    // cache is content addressed and a failure should say WHICH byte moved.
    const std::string document = rig_document(sample_bones());
    const ImportResult first = import_document(document);
    const ImportResult second = import_document(document);
    const SubAsset* left = find_skeleton(first);
    const SubAsset* right = find_skeleton(second);
    CY_REQUIRE(left != nullptr);
    CY_REQUIRE(right != nullptr);
    CY_CHECK(left->view() == right->view());
    CY_REQUIRE_EQ(left->payload.size(), right->payload.size());
    for (usize at = 0; at < left->payload.size(); ++at) {
        CY_REQUIRE_EQ(left->payload[at], right->payload[at]);
    }
}

CY_TEST_CASE("fbx skeleton: a file with no bones produces no skeleton and no warning") {
    // "A format that cannot express a step SHALL skip it and say so in the import report. A step
    // skipped for that reason is not a warning about the file." A static model HAS no rig, and an
    // importer that warned about it would train a project to ignore its warnings.
    const ImportResult result = import_document(rig_document({}));
    CY_CHECK(find_skeleton(result) == nullptr);
    for (const ImportDiagnostic& diagnostic : result.diagnostics()) {
        CY_CHECK(std::string_view(diagnostic.code) != "skeleton");
        CY_CHECK(std::string_view(diagnostic.code) != "skeleton-too-large");
    }
}

CY_TEST_CASE("fbx skeleton: the reader refuses a record the runtime would refuse") {
    // The record's whole purpose is that loading it cannot fail, so the reader makes the runtime's
    // three refusals where it can still name the joint. A forward parent reference is the one that
    // matters most: `Skeleton::add_joint` refuses it, and a package carrying one would fail at load
    // time in somebody else's build.
    const ImportResult result = import_document(rig_document(sample_bones()));
    const SubAsset* produced = find_skeleton(result);
    CY_REQUIRE(produced != nullptr);
    std::vector<u8> bytes(produced->payload.begin(), produced->payload.end());

    ImportedSkeleton reference;
    CY_REQUIRE(read_cooked_skeleton(cy::Span<const u8>(bytes.data(), bytes.size()), reference)
                   .has_value());

    // Joint 1's parent field: the 12-byte header, the skeleton name, joint 0's whole record, then
    // joint 1's name length and name. The offsets are walked from the record's own field sizes and
    // the names the reader gave back, so a layout change moves this cursor with it rather than
    // leaving a magic number pointing at the wrong field.
    constexpr usize kHeaderBytes = 12;
    constexpr usize kNameLengthBytes = 4;
    constexpr usize kParentBytes = 4;
    constexpr usize kTransformBytes = usize{10} * 4;  // translation, rotation, scale
    constexpr usize kBoneLevelBytes = 4;
    constexpr usize kJointFixedBytes = kParentBytes + kTransformBytes + kBoneLevelBytes;
    usize cursor = kHeaderBytes + reference.name.size();
    cursor += kNameLengthBytes + reference.joints[0].name.size() + kJointFixedBytes;
    cursor += kNameLengthBytes + reference.joints[1].name.size();
    bytes[cursor] = 3;  // parent = joint 3, ahead of it
    ImportedSkeleton broken;
    CY_CHECK_FALSE(
        read_cooked_skeleton(cy::Span<const u8>(bytes.data(), bytes.size()), broken).has_value());
}

// --- A real Mixamo export ------------------------------------------------------------------------

CY_TEST_CASE("fbx skeleton: a Mixamo character's 65-joint rig imports and loads") {
    std::vector<u8> bytes;
    if (!read_sample("Walking.fbx", bytes)) {
        CY_TEST_MESSAGE(
            "skipped: set CY_IMPORT_FBX_SAMPLES to a directory holding Walking.fbx to run this "
            "case. It is a 16.7 MB Mixamo export and cannot be committed.");
        return;
    }

    // Meshes and materials off: this case is about step 7, and welding and optimising a 14,634
    // vertex character would spend the suite's whole budget on the steps `test_fbx.cpp` covers.
    ImportOptions options;
    const OptionsSchema schema = fbx_options();
    CY_REQUIRE(options.set(schema, "import-meshes", OptionValue::of_bool(false)).has_value());
    CY_REQUIRE(options.set(schema, "import-materials", OptionValue::of_bool(false)).has_value());
    const ImportResult result = import_bytes(cy::Span<const u8>(bytes.data(), bytes.size()),
                                             "models/Walking.fbx", &options);

    const SubAsset* produced = find_skeleton(result);
    CY_REQUIRE(produced != nullptr);
    CY_CHECK(produced->view() == "skeleton/Hips");
    const ImportedSkeleton record = skeleton_of(*produced);

    // 65 bones: the Mixamo rig, hips to fingertips, measured on the file itself.
    CY_CHECK_EQ(record.joints.size(), usize{65});
    for (usize index = 0; index < record.joints.size(); ++index) {
        CY_REQUIRE(record.joints[index].parent < static_cast<i32>(index));
    }
    // The mesh node `Ch45` is a SIBLING of the skeleton root, not an ancestor of it, and it is not
    // a bone — so it must not have been swept into the rig.
    CY_CHECK_EQ(record.find("Ch45"), -1);
    CY_CHECK_NE(record.find("mixamorig1:Hips"), -1);
    CY_CHECK_NE(record.find("mixamorig1:LeftHandPinky4"), -1);

    // Every standard humanoid joint this rig has a bone for is mapped: 22 of 22, because Mixamo's
    // naming covers the whole profile and the root slot falls to the hips.
    CY_CHECK_EQ(record.humanoid.mapped_count(), u32{22});
    CY_CHECK_EQ(record.humanoid.resolve(1), record.find("mixamorig1:Hips"));
    CY_CHECK_EQ(record.humanoid.resolve(8), record.find("mixamorig1:LeftForeArm"));
    CY_CHECK_EQ(record.humanoid.resolve(20), record.find("mixamorig1:RightFoot"));
    CY_CHECK_EQ(record.humanoid.resolve(17), record.find("mixamorig1:LeftToeBase"));

    // The forty finger joints and the three chain terminators — `HeadTop_End` and the two
    // `Toe_End`s — leave at bone level 1; the spine, the limbs and the feet never do.
    u32 dropped = 0;
    for (const ImportedJoint& joint : record.joints) {
        dropped += joint.dropped_at < kBoneLodLevels ? 1U : 0U;
    }
    CY_CHECK_EQ(dropped, u32{43});

    cy::animation::Skeleton skeleton(allocator());
    cy::animation::SkeletonProfile profile;
    CY_REQUIRE(build_runtime_skeleton(record, skeleton, profile).has_value());
    CY_REQUIRE(skeleton.finalized());
    CY_REQUIRE_EQ(skeleton.joint_count(), u16{65});

    cy::Array<cy::Transform> local(allocator());
    cy::Array<cy::Transform> model(allocator());
    CY_REQUIRE(local.resize(65).has_value());
    CY_REQUIRE(model.resize(65).has_value());
    skeleton.reference_pose(local.span());
    skeleton.to_model(local.span(), skeleton.retained(0), model.span());
    for (u16 joint = 0; joint < 65; ++joint) {
        CY_CHECK_NEAR(model[joint].translation.x, skeleton.bind_model()[joint].translation.x, 1e-5);
        CY_CHECK_NEAR(model[joint].translation.y, skeleton.bind_model()[joint].translation.y, 1e-5);
        CY_CHECK_NEAR(model[joint].translation.z, skeleton.bind_model()[joint].translation.z, 1e-5);
    }

    // A human being, in metres: the hips sit at about a metre and the head above a metre and a
    // half. A file whose centimetres were not converted puts the hips at 99.
    const u16 hips = profile.resolve(cy::animation::HumanoidJoint::Hips);
    const u16 head = profile.resolve(cy::animation::HumanoidJoint::Head);
    CY_REQUIRE(hips != cy::animation::kInvalidJoint);
    CY_REQUIRE(head != cy::animation::kInvalidJoint);
    CY_CHECK_NEAR(model[hips].translation.y, 0.99f, 0.05);
    CY_CHECK_GT(model[head].translation.y, 1.4f);
    CY_CHECK_LT(model[head].translation.y, 1.9f);

    // The bone level of detail is a real reduction on a real rig, and it is nested — finalize()
    // would have refused it otherwise.
    CY_CHECK_EQ(skeleton.retained_count(0), u32{65});
    CY_CHECK_EQ(skeleton.retained_count(1), u32{65} - dropped);
}

CY_TEST_CASE("fbx skeleton: an animation-only Mixamo export carries the same rig") {
    // Each Mixamo file ships its own copy of the skeleton, so an animation file with no mesh and no
    // skin still has 65 bones — and `AnimationRig::bind` checks only joint COUNTS, never identity,
    // so whoever binds a clip from one file against a skeleton from another needs this to be true.
    std::vector<u8> bytes;
    if (!read_sample("Breathing Idle.fbx", bytes)) {
        CY_TEST_MESSAGE(
            "skipped: set CY_IMPORT_FBX_SAMPLES to a directory holding 'Breathing Idle.fbx'.");
        return;
    }
    const ImportResult result =
        import_bytes(cy::Span<const u8>(bytes.data(), bytes.size()), "models/idle.fbx", nullptr);
    const SubAsset* produced = find_skeleton(result);
    CY_REQUIRE(produced != nullptr);
    const ImportedSkeleton record = skeleton_of(*produced);
    CY_CHECK_EQ(record.joints.size(), usize{65});
    CY_CHECK_EQ(record.humanoid.mapped_count(), u32{22});
    for (usize index = 0; index < record.joints.size(); ++index) {
        CY_REQUIRE(record.joints[index].parent < static_cast<i32>(index));
    }
}

// --- The half of step 7 that lives on the mesh. M11.b task 6.1. ----------------------------------

namespace {

/// The cooked render mesh of an import, or null.
const SubAsset* find_mesh(const ImportResult& result) {
    for (const SubAsset& asset : result.assets()) {
        if (asset.kind == cy::assets::AssetKind::Mesh && asset.view().starts_with("mesh/")) {
            return &asset;
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

/// The binding of the vertex at `(x, y)` in METRES, found by position rather than by index: the
/// weld, the cache reorder and the fetch reorder all permute the buffer, and an assertion by index
/// would pass on a mesh whose bindings had all been swapped.
const SkinInfluence* binding_at(const MeshData& mesh, f32 x, f32 y) {
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        if (cy::math::nearly_equal(mesh.positions[index].x, x, 1e-4f) &&
            cy::math::nearly_equal(mesh.positions[index].y, y, 1e-4f)) {
            return &mesh.skin[index];
        }
    }
    return nullptr;
}

/// The joint index a weighted slot names, or -1 when the slot is unused.
i32 joint_of_weight(const SkinInfluence& influence, f32 weight) {
    for (usize slot = 0; slot < kSkinInfluences; ++slot) {
        if (cy::math::nearly_equal(influence.weights[slot], weight, 1e-4f)) {
            return static_cast<i32>(influence.joints[slot]);
        }
    }
    return -1;
}

f32 weight_sum(const SkinInfluence& influence) {
    f32 total = 0.0f;
    for (const f32 weight : influence.weights) {
        total += weight;
    }
    return total;
}

}  // namespace

CY_TEST_CASE("fbx skin: a cluster's weights reach the cooked mesh, on the right vertex") {
    // WHAT THIS IS THE REGRESSION FOR. Before M11.b this importer read every skin cluster and threw
    // it away, because `MeshData` had no joint or weight array — `samples/09b-animated-character`
    // derives its weights from vertex height for exactly that reason.
    const ImportResult result = import_document(skinned_document());
    CY_CHECK(!result.has_errors());

    const SubAsset* produced = find_mesh(result);
    CY_REQUIRE(produced != nullptr);
    const MeshData mesh = mesh_of(*produced);
    CY_REQUIRE_EQ(mesh.vertex_count(), usize{4});
    CY_REQUIRE_EQ(mesh.skin.size(), mesh.vertex_count());

    // The document is in centimetres — `UnitScaleFactor` is 1 — so a metre position is a hundredth
    // of the number the fixture writes.
    //
    // Vertex 0 is rigid to cluster 1, which deforms through `rig:Hips` — joint 0 of the walk order,
    // and NOT 1, which is the cluster's own index.
    const SkinInfluence* rigid = binding_at(mesh, 0.0f, 0.0f);
    CY_REQUIRE(rigid != nullptr);
    CY_CHECK_EQ(rigid->joints[0], u16{0});
    CY_CHECK_NEAR(rigid->weights[0], 1.0f, 1e-5);

    // Vertex 2 is rigid to cluster 0, which deforms through `rig:LeftHandIndex1` — joint 3. This is
    // the vertex that tells a cluster index from a joint index, and it is also the SECOND
    // triangle's first corner, which is what tells a corner index from a vertex index.
    const SkinInfluence* leaf = binding_at(mesh, 0.0f, 1.0f);
    CY_REQUIRE(leaf != nullptr);
    CY_CHECK_EQ(leaf->joints[0], u16{3});
    CY_CHECK_NEAR(leaf->weights[0], 1.0f, 1e-5);

    // Vertex 1 is the blend: half through `rig:Spine` (joint 1) and half through `rig:LeftHand`
    // (joint 2), from clusters 2 and 3.
    const SkinInfluence* blended = binding_at(mesh, 1.0f, 0.0f);
    CY_REQUIRE(blended != nullptr);
    CY_CHECK_NEAR(weight_sum(*blended), 1.0f, 1e-5);
    const bool spine = joint_of_weight(*blended, 0.5f) == 1 || blended->joints[1] == 1;
    CY_CHECK(spine);
    CY_CHECK(blended->joints[0] != blended->joints[1]);

    // And the refusal this rung removed: a skinned file no longer reports that its skins were
    // skipped.
    for (const ImportDiagnostic& diagnostic : result.diagnostics()) {
        if (std::string_view(diagnostic.code) == "skipped-rig") {
            const bool names_skins =
                std::string_view(diagnostic.detail).find("skins") != std::string_view::npos;
            CY_CHECK(!names_skins);
        }
    }
}

CY_TEST_CASE("fbx skin: a fifth influence is dropped, the rest renormalised, and it is reported") {
    // `kSkinInfluences` is four, which is what every cooked vertex layout in this tree carries. A
    // vertex named by five clusters keeps the four heaviest — ufbx sorts a vertex's weights by
    // decreasing weight, so the prefix IS the heaviest four — and they are renormalised, because a
    // vertex whose weights sum to 0.9 shrinks towards the origin under skinning. That reads as a
    // seam rather than as a weight problem, which is why the reduction is also a diagnostic.
    const ImportResult result = import_document(skinned_document());
    const SubAsset* produced = find_mesh(result);
    CY_REQUIRE(produced != nullptr);
    const MeshData mesh = mesh_of(*produced);

    const SkinInfluence* crowded = binding_at(mesh, 1.0f, 1.0f);
    CY_REQUIRE(crowded != nullptr);
    // The five authored weights are 0.3, 0.25, 0.2, 0.15 and 0.1; the smallest goes and the other
    // four are scaled by 1/0.9. Without the renormalisation this sums to 0.9.
    CY_CHECK_NEAR(weight_sum(*crowded), 1.0f, 1e-4);
    CY_CHECK_NEAR(crowded->weights[0], 0.3f / 0.9f, 1e-4);
    CY_CHECK_NEAR(crowded->weights[3], 0.15f / 0.9f, 1e-4);
    // `rig:Head` is joint 4 and carried the dropped 0.1, so no slot names it.
    for (const u16 joint : crowded->joints) {
        CY_CHECK(joint != 4);
    }

    bool reported = false;
    for (const ImportDiagnostic& diagnostic : result.diagnostics()) {
        reported = reported || std::string_view(diagnostic.code) == "skin-influences-reduced";
    }
    CY_CHECK(reported);
}

CY_TEST_CASE("fbx skin: every joint a binding names is one the skeleton record carries") {
    // The two numberings — the file's cluster order and the skeleton record's walk order — are
    // joined by the bone node's NAME and by nothing else. An importer that used the cluster index
    // as the joint index is a character whose arm moves when its leg does.
    const ImportResult result = import_document(skinned_document());
    const SubAsset* skeleton_asset = find_skeleton(result);
    CY_REQUIRE(skeleton_asset != nullptr);
    const ImportedSkeleton record = skeleton_of(*skeleton_asset);
    CY_REQUIRE_EQ(record.joints.size(), usize{5});

    const SubAsset* produced = find_mesh(result);
    CY_REQUIRE(produced != nullptr);
    const MeshData mesh = mesh_of(*produced);
    CY_REQUIRE(!mesh.skin.empty());
    for (const SkinInfluence& influence : mesh.skin) {
        for (usize slot = 0; slot < kSkinInfluences; ++slot) {
            if (influence.weights[slot] <= 0.0f) {
                continue;
            }
            CY_REQUIRE(influence.joints[slot] < record.joints.size());
            const std::string& name = record.joints[influence.joints[slot]].name;
            const bool named_by_a_cluster = std::string_view(name).starts_with("rig:");
            CY_CHECK(named_by_a_cluster);
        }
    }
}

CY_TEST_CASE("fbx skin: a skinned import is byte-identical when it is run twice") {
    // `asset-import-pipeline` requires byte-identical output for identical input, and the cooked
    // mesh grew an attribute this milestone. A weight ordering that depended on a pointer would
    // make every cook cache miss.
    const std::string document = skinned_document();
    const ImportResult first = import_document(document);
    const ImportResult second = import_document(document);
    CY_REQUIRE_EQ(first.assets().size(), second.assets().size());
    for (usize index = 0; index < first.assets().size(); ++index) {
        CY_REQUIRE_EQ(first.assets()[index].payload.size(), second.assets()[index].payload.size());
        CY_CHECK_EQ(
            std::memcmp(first.assets()[index].payload.data(), second.assets()[index].payload.data(),
                        first.assets()[index].payload.size()),
            0);
    }
}
