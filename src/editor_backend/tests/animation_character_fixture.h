// SPDX-License-Identifier: MIT
#pragma once
// A project character the way a project gets one: an FBX document imported by the real importer.
// Shared by the animation panel's engine suite and the editor window runtime's Play suites.
//
// THE DOCUMENT is built in code, as tools/import's FBX suites build theirs: three bones (Hips,
// Spine, Head, in centimetres, which the importer turns into metres), a two-quad strip skinned
// one row of vertices to each bone, and ONE animation stack named `mixamo.com` that turns the Spine
// about Z from 0 to 45 degrees over a second. The stack's name is the one every Mixamo export
// carries, which is why a clip is named by its sub-asset (`animation/hero` -> `hero`) and not by
// what the cooked clip calls itself.
//
// `import_character` runs `cy::import::FbxImporter` over it and keeps the three sub-assets a
// character is made of, by kind of name: the skeleton, the clip and the skinned mesh.

#include <cy/core/assets/identity.h>
#include <cy/import/fbx.h>
#include <cy/import/fbx_skeleton.h>
#include <cy/test/test.h>

#include <string>
#include <string_view>
#include <vector>

namespace cy::editor::testing {

/// FBX's time unit: one second is this many KTime ticks.
inline constexpr long long kCharacterSecond = 46186158000LL;

/// The character's FBX, as described above. `degrees` is where the Spine's turn ends; `prefix` is
/// put before every bone's name, so two documents can rig two skeletons that do not match.
inline std::string character_document(double degrees = 45.0, std::string_view prefix = "") {
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
    struct Bone {
        const char* name;
        int parent;
        int y;
    };
    const Bone bones[] = {{"Hips", -1, 100}, {"Spine", 0, 30}, {"Head", 1, 30}};
    for (int index = 0; index < 3; ++index) {
        const std::string id = std::to_string(2000 + index);
        const std::string attribute = std::to_string(3000 + index);
        const std::string bone = std::string(prefix) + bones[index].name;
        text.append("\tNodeAttribute: ").append(attribute).append(", \"NodeAttribute::");
        text.append(bone).append("\", \"LimbNode\" {\n\t\tTypeFlags: \"Skeleton\"\n\t}\n");
        text.append("\tModel: ").append(id).append(", \"Model::").append(bone);
        text.append("\", \"LimbNode\" {\n\t\tVersion: 232\n\t\tProperties70:  {\n");
        text += "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0," +
                std::to_string(bones[index].y) + ",0\n\t\t}\n\t}\n";
    }
    text +=
        "\tGeometry: 4000, \"Geometry::Body\", \"Mesh\" {\n"
        "\t\tVertices: *18 {\n"
        "\t\t\ta: -10,100,0,10,100,0,-10,130,0,10,130,0,-10,160,0,10,160,0\n"
        "\t\t}\n"
        "\t\tPolygonVertexIndex: *8 {\n\t\t\ta: 0,1,3,-3,2,3,5,-5\n\t\t}\n"
        "\t\tGeometryVersion: 124\n"
        "\t}\n"
        "\tModel: 4100, \"Model::Body\", \"Mesh\" {\n\t\tVersion: 232\n\t}\n"
        "\tDeformer: 5000, \"Deformer::Skin\", \"Skin\" {\n"
        "\t\tVersion: 101\n\t\tLink_DeformAcuracy: 50\n\t}\n";
    for (int bone = 0; bone < 3; ++bone) {
        const std::string id = std::to_string(5001 + bone);
        text += "\tDeformer: " + id + ", \"SubDeformer::Cluster" + bones[bone].name +
                "\", \"Cluster\" {\n\t\tVersion: 100\n\t\tUserData: \"\", \"\"\n";
        text += "\t\tIndexes: *2 {\n\t\t\ta: " + std::to_string(bone * 2) + "," +
                std::to_string((bone * 2) + 1) + "\n\t\t}\n";
        text += "\t\tWeights: *2 {\n\t\t\ta: 1,1\n\t\t}\n";
        text +=
            "\t\tTransform: *16 {\n\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1\n\t\t}\n"
            "\t\tTransformLink: *16 {\n\t\t\ta: 1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1\n\t\t}\n"
            "\t}\n";
    }
    text +=
        "\tAnimationStack: 6000, \"AnimStack::mixamo.com\", \"\" {\n"
        "\t\tProperties70:  {\n"
        "\t\t\tP: \"LocalStart\", \"KTime\", \"Time\", \"\",0\n";
    text += "\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\"," +
            std::to_string(kCharacterSecond) + "\n\t\t}\n\t}\n";
    text +=
        "\tAnimationLayer: 6100, \"AnimLayer::BaseLayer\", \"\" {\n\t}\n"
        "\tAnimationCurveNode: 6200, \"AnimCurveNode::R\", \"\" {\n"
        "\t\tProperties70:  {\n"
        "\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n"
        "\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\",0\n"
        "\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0\n"
        "\t\t}\n"
        "\t}\n"
        "\tAnimationCurve: 6300, \"AnimCurve::\", \"\" {\n"
        "\t\tDefault: 0\n"
        "\t\tKeyVer: 4009\n";
    text += "\t\tKeyTime: *2 {\n\t\t\ta: 0," + std::to_string(kCharacterSecond) + "\n\t\t}\n";
    text += "\t\tKeyValueFloat: *2 {\n\t\t\ta: 0," + std::to_string(degrees) + "\n\t\t}\n";
    text +=
        "\t\tKeyAttrFlags: *1 {\n\t\t\ta: 4\n\t\t}\n"
        "\t\tKeyAttrDataFloat: *4 {\n\t\t\ta: 0,0,0,0\n\t\t}\n"
        "\t\tKeyAttrRefCount: *1 {\n\t\t\ta: 2\n\t\t}\n"
        "\t}\n"
        "}\n";
    text += "Connections:  {\n";
    for (int index = 0; index < 3; ++index) {
        const std::string id = std::to_string(2000 + index);
        const std::string parent =
            bones[index].parent < 0 ? "0" : std::to_string(2000 + bones[index].parent);
        text.append("\tC: \"OO\",").append(id).append(",").append(parent).append("\n");
        text += "\tC: \"OO\"," + std::to_string(3000 + index) + "," + id + "\n";
    }
    text +=
        "\tC: \"OO\",4100,0\n"
        "\tC: \"OO\",4000,4100\n"
        "\tC: \"OO\",5000,4000\n";
    for (int bone = 0; bone < 3; ++bone) {
        const std::string id = std::to_string(5001 + bone);
        text += "\tC: \"OO\"," + id + ",5000\n";
        text += "\tC: \"OO\"," + std::to_string(2000 + bone) + "," + id + "\n";
    }
    text +=
        "\tC: \"OO\",6100,6000\n"
        "\tC: \"OO\",6200,6100\n"
        "\tC: \"OP\",6200,2001,\"Lcl Rotation\"\n"
        "\tC: \"OP\",6300,6200,\"d|Z\"\n"
        "}\n";
    return text;
}

/// One cooked sub-asset of the imported character.
struct CookedPart {
    std::string name;
    assets::AssetKind kind = assets::AssetKind::Unknown;
    std::vector<u8> payload;
};

/// The character's skeleton, clip and skinned mesh, as the importer cooked them.
struct ImportedCharacter {
    CookedPart skeleton;
    CookedPart clip;
    CookedPart mesh;
};

inline ImportedCharacter import_character(double degrees = 45.0,
                                          std::string_view source = "characters/hero.fbx",
                                          std::string_view prefix = "") {
    const std::string text = character_document(degrees, prefix);
    import::FbxImporter importer;
    import::ImportRequest request;
    request.source = assets::VirtualPath::normalise(source).value();
    request.bytes = Span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size());
    import::ImportResult result;
    CY_REQUIRE(importer.import(request, result).has_value());
    CY_REQUIRE_FALSE(result.has_errors());
    ImportedCharacter out;
    for (const import::SubAsset& produced : result.assets()) {
        const std::string_view name = produced.view();
        CookedPart* part = nullptr;
        if (name.starts_with(import::kSkeletonSubAssetPrefix)) {
            part = &out.skeleton;
        } else if (name.starts_with("animation/")) {
            part = &out.clip;
        } else if (name.starts_with("mesh/")) {
            part = &out.mesh;
        }
        if (part != nullptr) {
            part->name = std::string(name);
            part->kind = produced.kind;
            part->payload.assign(produced.payload.begin(), produced.payload.end());
        }
    }
    CY_REQUIRE_FALSE(out.skeleton.payload.empty());
    CY_REQUIRE_FALSE(out.clip.payload.empty());
    CY_REQUIRE_FALSE(out.mesh.payload.empty());
    return out;
}

}  // namespace cy::editor::testing
