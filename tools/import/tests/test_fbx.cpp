// FBX import, end to end in memory. M6 task 8.1.
//
// The documents are built here rather than checked in as fixtures, for the reason `test_gltf.cpp`
// gives: a binary fixture is a file somebody has to maintain and nobody can read in a review.
//
// THE FILES BELOW ARE ASCII FBX 7.4, which is a format ufbx reads and a person can. A binary FBX
// would exercise the same code path in this importer — ufbx normalises both into one scene — and
// would cost a checked-in blob to say the same thing. What binary parsing is being trusted to do is
// upstream's job and upstream's test suite; what is being asserted here is everything this
// repository owns: the coordinate and unit conversion, the section split by material, the standard
// material mapping, the collision naming convention, the hierarchy, and determinism.

#include <cy/core/math/scalar.h>
#include <cy/import/fbx.h>
#include <cy/import/model.h>
#include <cy/test/test.h>

#include <string>
#include <string_view>
#include <vector>

using namespace cy::import;
using cy::f32;
using cy::u8;
using cy::usize;

namespace {

/// An ASCII FBX holding one quad in the XY plane — two triangles, four vertices, normals, texture
/// coordinates and one material — under a node the caller names and places.
///
/// `up_axis` is 1 for Y-up and 2 for Z-up; `unit_scale_factor` is the file's declared unit in
/// centimetres, so 1 is a centimetre file and 100 a metre file. Both are the FBX header fields real
/// exporters write, and both are what the importer's conversion is driven by.
std::string quad_document(std::string_view node_name, int up_axis, double unit_scale_factor,
                          bool second_node) {
    std::string text =
        "; FBX 7.4.0 project file\n"
        "FBXHeaderExtension:  {\n"
        "\tFBXHeaderVersion: 1003\n"
        "\tFBXVersion: 7400\n"
        "}\n"
        "GlobalSettings:  {\n"
        "\tVersion: 1000\n"
        "\tProperties70:  {\n";
    text += "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\"," + std::to_string(up_axis) + "\n";
    text += "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n";
    // The two axis systems real exporters write. Z-up is 3ds Max's — up Z, front −Y, right X — and
    // Y-up is Maya's and glTF's. Both are right-handed; a sign flip here would make the file
    // left-handed, and ufbx would then correctly mirror the scene, which is a different test.
    text += up_axis == 2 ? "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",1\n"
                         : "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n";
    text += up_axis == 2 ? "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",-1\n"
                         : "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n";
    text += "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n";
    text += "\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n";
    text += "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\"," +
            std::to_string(unit_scale_factor) + "\n";
    text +=
        "\t}\n"
        "}\n"
        "Objects:  {\n"
        "\tGeometry: 1000, \"Geometry::quad\", \"Mesh\" {\n"
        "\t\tVertices: *12 {\n"
        "\t\t\ta: 0,0,0,1,0,0,1,1,0,0,1,0\n"
        "\t\t}\n"
        // Two triangles. Each face's LAST index is bitwise-negated, which is how FBX terminates a
        // polygon; getting that wrong produces one six-cornered face rather than two triangles,
        // which is a mistake worth naming here because the file still loads.
        "\t\tPolygonVertexIndex: *6 {\n"
        "\t\t\ta: 0,1,-3,0,2,-4\n"
        "\t\t}\n"
        "\t\tGeometryVersion: 124\n"
        "\t\tLayerElementNormal: 0 {\n"
        "\t\t\tVersion: 102\n"
        "\t\t\tName: \"\"\n"
        "\t\t\tMappingInformationType: \"ByPolygonVertex\"\n"
        "\t\t\tReferenceInformationType: \"Direct\"\n"
        "\t\t\tNormals: *18 {\n"
        "\t\t\t\ta: 0,0,1,0,0,1,0,0,1,0,0,1,0,0,1,0,0,1\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tLayerElementUV: 0 {\n"
        "\t\t\tVersion: 101\n"
        "\t\t\tName: \"map1\"\n"
        "\t\t\tMappingInformationType: \"ByPolygonVertex\"\n"
        "\t\t\tReferenceInformationType: \"Direct\"\n"
        "\t\t\tUV: *12 {\n"
        "\t\t\t\ta: 0,0,1,0,1,1,0,0,1,1,0,1\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tLayerElementMaterial: 0 {\n"
        "\t\t\tVersion: 101\n"
        "\t\t\tName: \"\"\n"
        "\t\t\tMappingInformationType: \"AllSame\"\n"
        "\t\t\tReferenceInformationType: \"IndexToDirect\"\n"
        "\t\t\tMaterials: *1 {\n"
        "\t\t\t\ta: 0\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t\tLayer: 0 {\n"
        "\t\t\tVersion: 100\n"
        "\t\t\tLayerElement:  {\n"
        "\t\t\t\tType: \"LayerElementNormal\"\n"
        "\t\t\t\tTypedIndex: 0\n"
        "\t\t\t}\n"
        "\t\t\tLayerElement:  {\n"
        "\t\t\t\tType: \"LayerElementUV\"\n"
        "\t\t\t\tTypedIndex: 0\n"
        "\t\t\t}\n"
        "\t\t\tLayerElement:  {\n"
        "\t\t\t\tType: \"LayerElementMaterial\"\n"
        "\t\t\t\tTypedIndex: 0\n"
        "\t\t\t}\n"
        "\t\t}\n"
        "\t}\n";
    text += "\tModel: 2000, \"Model::";
    text += node_name;
    text +=
        "\", \"Mesh\" {\n"
        "\t\tVersion: 232\n"
        "\t\tProperties70:  {\n"
        "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,2\n"
        "\t\t}\n"
        "\t}\n";
    if (second_node) {
        text +=
            "\tModel: 2001, \"Model::Child\", \"Null\" {\n"
            "\t\tVersion: 232\n"
            "\t\tProperties70:  {\n"
            "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,1\n"
            "\t\t}\n"
            "\t}\n";
    }
    text +=
        "\tMaterial: 3000, \"Material::Oak\", \"\" {\n"
        "\t\tVersion: 102\n"
        "\t\tShadingModel: \"phong\"\n"
        "\t\tProperties70:  {\n"
        "\t\t\tP: \"DiffuseColor\", \"Color\", \"\", \"A\",0.8,0.6,0.4\n"
        "\t\t}\n"
        "\t}\n"
        "}\n"
        "Connections:  {\n"
        "\tC: \"OO\",2000,0\n"
        "\tC: \"OO\",1000,2000\n"
        "\tC: \"OO\",3000,2000\n";
    if (second_node) {
        text += "\tC: \"OO\",2001,2000\n";
    }
    text += "}\n";
    return text;
}

ImportResult import_document(const std::string& text, const ImportOptions* options) {
    FbxImporter importer;
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise("models/quad.fbx").value();
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

std::vector<ImportedNode> nodes_of(const SubAsset& prefab, cy::Array<char>& names) {
    cy::Array<ImportedNode> read;
    CY_REQUIRE(read_cooked_scene_graph(
                   cy::Span<const u8>(prefab.payload.data(), prefab.payload.size()), read, names)
                   .has_value());
    return {read.begin(), read.end()};
}

}  // namespace

CY_TEST_CASE("fbx: the importer declares itself, and its schema matches the glTF importer's") {
    const FbxImporter importer;
    const ImporterInfo info = importer.info();
    CY_CHECK(info.name == "fbx");
    CY_CHECK(info.extensions.size() == 1);
    CY_CHECK(info.extensions[0] == ".fbx");

    // The two model importers declare their shared options under the SAME names, so a project that
    // re-exports a model in the other format keeps its import settings. This asserts it rather than
    // leaving it to a comment: the two schemas are edited in two files.
    const OptionsSchema fbx = fbx_options();
    const OptionsSchema gltf = gltf_options();
    static constexpr std::string_view kShared[] = {"scale",
                                                   "source-up-axis",
                                                   "import-meshes",
                                                   "import-materials",
                                                   "weld-tolerance",
                                                   "smoothing-angle",
                                                   "generate-tangents",
                                                   "optimise",
                                                   "overdraw-threshold",
                                                   "generate-lightmap-uvs",
                                                   "lightmap-texel-density",
                                                   "lightmap-padding",
                                                   "lod-count",
                                                   "lod-ratio",
                                                   "lod-error-bound",
                                                   "collision-suffix",
                                                   "collision-mode"};
    for (const std::string_view name : kShared) {
        CY_CHECK(fbx.find(name) != nullptr);
        CY_CHECK(gltf.find(name) != nullptr);
    }
}

CY_TEST_CASE("fbx: a document produces a mesh, a material and a prefab") {
    const ImportResult result = import_document(quad_document("Panel", 1, 100.0, false), nullptr);
    CY_CHECK(!result.has_errors());

    const SubAsset* mesh = find(result, "mesh/quad");
    CY_REQUIRE(mesh != nullptr);
    CY_CHECK(mesh->kind == cy::assets::AssetKind::Mesh);

    const SubAsset* material = find(result, "material/Oak");
    CY_REQUIRE(material != nullptr);
    CY_CHECK(material->kind == cy::assets::AssetKind::Material);

    const SubAsset* prefab = find(result, "prefab");
    CY_REQUIRE(prefab != nullptr);
    CY_CHECK(prefab->primary);
    usize primaries = 0;
    for (const SubAsset& produced : result.assets()) {
        primaries += produced.primary ? 1U : 0U;
    }
    CY_CHECK(primaries == 1);
}

CY_TEST_CASE("fbx: the mesh comes through welded, with tangents generated") {
    const ImportResult result = import_document(quad_document("Panel", 1, 100.0, false), nullptr);
    const MeshData mesh = mesh_of(*find(result, "mesh/quad"));

    // FBX stores attributes per CORNER, so the quad arrives as six vertices and the weld is what
    // brings it back to four. That is the step this format needs and glTF does not.
    CY_CHECK(mesh.triangle_count() == 2);
    CY_CHECK(mesh.vertex_count() == 4);
    CY_CHECK(mesh.normals.size() == mesh.vertex_count());
    CY_CHECK(mesh.uvs.size() == mesh.vertex_count());
    CY_CHECK(mesh.tangents.size() == mesh.vertex_count());
    CY_CHECK(mesh.sections.size() == 1);
}

CY_TEST_CASE("fbx: the file's declared unit is converted to metres") {
    // `UnitScaleFactor` is the file's unit in centimetres, so 1 is a centimetre file. A quad one
    // unit across must arrive one centimetre across, because the engine's unit is the metre
    // (`core-math`) and no runtime code accounts for a source's unit.
    const ImportResult centimetres =
        import_document(quad_document("Panel", 1, 1.0, false), nullptr);
    const MeshData small = mesh_of(*find(centimetres, "mesh/quad"));
    f32 widest = 0.0f;
    for (usize index = 0; index < small.vertex_count(); ++index) {
        widest = small.positions[index].x > widest ? small.positions[index].x : widest;
    }
    CY_CHECK(cy::math::nearly_equal(widest, 0.01f, 1e-5f));

    const ImportResult metres = import_document(quad_document("Panel", 1, 100.0, false), nullptr);
    const MeshData large = mesh_of(*find(metres, "mesh/quad"));
    widest = 0.0f;
    for (usize index = 0; index < large.vertex_count(); ++index) {
        widest = large.positions[index].x > widest ? large.positions[index].x : widest;
    }
    CY_CHECK(cy::math::nearly_equal(widest, 1.0f, 1e-5f));
}

CY_TEST_CASE("fbx: a z-up source is converted at import") {
    // `asset-import-pipeline`: "WHEN a Z-up model is imported THEN it SHALL be converted at import
    // so no runtime code accounts for source handedness."
    //
    // The conversion lands in the NODE transform rather than in the vertices, which is where a
    // change of coordinate frame belongs — see fbx.cpp. So the assertion is on the node: the
    // source's `Lcl Translation` of (0, 0, 2) is two units along ITS up axis, and after conversion
    // it must be two metres along the engine's, which is +Y.
    const ImportResult result = import_document(quad_document("Panel", 2, 100.0, false), nullptr);
    const SubAsset* prefab = find(result, "prefab");
    CY_REQUIRE(prefab != nullptr);
    cy::Array<char> names;
    const std::vector<ImportedNode> nodes = nodes_of(*prefab, names);
    CY_REQUIRE(nodes.size() == 1);
    CY_CHECK(cy::math::nearly_equal(nodes[0].translation.y, 2.0f, 1e-4f));
    CY_CHECK(cy::math::nearly_equal(nodes[0].translation.z, 0.0f, 1e-4f));

    // A Y-up file of the same shape places the same node along +Z, which is what the conversion is
    // distinguishing. Without it the two files would produce the same placement and one would be
    // wrong.
    const ImportResult upright = import_document(quad_document("Panel", 1, 100.0, false), nullptr);
    cy::Array<char> upright_names;
    const std::vector<ImportedNode> upright_nodes =
        nodes_of(*find(upright, "prefab"), upright_names);
    CY_REQUIRE(upright_nodes.size() == 1);
    CY_CHECK(cy::math::nearly_equal(upright_nodes[0].translation.z, 2.0f, 1e-4f));
}

CY_TEST_CASE("fbx: the scale option multiplies geometry and node translations together") {
    ImportOptions options;
    const OptionsSchema schema = fbx_options();
    CY_REQUIRE(options.set(schema, "scale", OptionValue::of_float(2.0)).has_value());

    const ImportResult result = import_document(quad_document("Panel", 1, 100.0, false), &options);
    const MeshData mesh = mesh_of(*find(result, "mesh/quad"));
    f32 widest = 0.0f;
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        widest = mesh.positions[index].x > widest ? mesh.positions[index].x : widest;
    }
    CY_CHECK(cy::math::nearly_equal(widest, 2.0f, 1e-4f));

    // The node's translation is scaled by the same number: a model whose geometry was scaled and
    // whose nodes were not would assemble itself wrongly, which is the defect this asserts against.
    cy::Array<char> names;
    const std::vector<ImportedNode> nodes = nodes_of(*find(result, "prefab"), names);
    CY_REQUIRE(nodes.size() == 1);
    CY_CHECK(cy::math::nearly_equal(nodes[0].translation.z, 4.0f, 1e-4f));
}

CY_TEST_CASE("fbx: the hierarchy is depth first, and a parent's index is below its children's") {
    const ImportResult result = import_document(quad_document("Panel", 1, 100.0, true), nullptr);
    cy::Array<char> names;
    const std::vector<ImportedNode> nodes = nodes_of(*find(result, "prefab"), names);
    CY_REQUIRE(nodes.size() == 2);
    CY_CHECK(nodes[0].name == "Panel");
    CY_CHECK(nodes[0].parent == -1);
    CY_CHECK(nodes[1].name == "Child");
    CY_CHECK(nodes[1].parent == 0);
    // The node that carries the mesh is the one the mesh was attached to, and the other draws
    // nothing.
    CY_CHECK(nodes[0].mesh == 0);
    CY_CHECK(nodes[1].mesh == -1);
}

CY_TEST_CASE("fbx: the standard material carries the source's own parameters") {
    const ImportResult result = import_document(quad_document("Panel", 1, 100.0, false), nullptr);
    const SubAsset* produced = find(result, "material/Oak");
    CY_REQUIRE(produced != nullptr);
    StandardMaterial material;
    CY_REQUIRE(read_cooked_material(
                   cy::Span<const u8>(produced->payload.data(), produced->payload.size()), material)
                   .has_value());
    CY_CHECK(cy::math::nearly_equal(material.base_colour[0], 0.8f, 1e-3f));
    CY_CHECK(cy::math::nearly_equal(material.base_colour[1], 0.6f, 1e-3f));
    CY_CHECK(cy::math::nearly_equal(material.base_colour[2], 0.4f, 1e-3f));
    // An opaque source is opaque: FBX has no alpha mode, so it is derived from the opacity, and a
    // material that arrived blended by accident would draw in the wrong pass.
    CY_CHECK(material.alpha_mode == 0);
}

CY_TEST_CASE("fbx: the collision naming convention produces a collider and hides the node") {
    // "WHEN a node is named with the configured collision suffix THEN a collider SHALL be generated
    // from it and the node excluded from rendering."
    const ImportResult result =
        import_document(quad_document("Crate_collision", 1, 100.0, false), nullptr);
    cy::Array<char> names;
    const std::vector<ImportedNode> nodes = nodes_of(*find(result, "prefab"), names);
    CY_REQUIRE(nodes.size() == 1);
    CY_CHECK(nodes[0].collision_only);
    CY_CHECK(nodes[0].mesh == -1);
    CY_CHECK(nodes[0].collision == 0);

    // A flat quad has no convex hull, so the importer says so and keeps the triangles rather than
    // producing nothing. Both halves of that are the requirement.
    const SubAsset* collider = find(result, "collision/Crate_collision");
    CY_REQUIRE(collider != nullptr);
    const MeshData mesh = mesh_of(*collider);
    CY_CHECK(mesh.triangle_count() >= 1);
    // A collider carries positions and topology and nothing else.
    CY_CHECK(mesh.normals.empty());
    CY_CHECK(mesh.uvs.empty());
    CY_CHECK(mesh.tangents.empty());
}

CY_TEST_CASE("fbx: a level-of-detail chain is generated to the configured count") {
    ImportOptions options;
    const OptionsSchema schema = fbx_options();
    CY_REQUIRE(options.set(schema, "lod-count", OptionValue::of_int(2)).has_value());

    const ImportResult result = import_document(quad_document("Panel", 1, 100.0, false), &options);
    CY_CHECK(find(result, "mesh/quad") != nullptr);
    CY_CHECK(find(result, "mesh/quad/lod1") != nullptr);
    CY_CHECK(find(result, "mesh/quad/lod2") != nullptr);
    CY_CHECK(find(result, "mesh/quad/lod3") == nullptr);
}

CY_TEST_CASE("fbx: two imports of one file produce byte-identical payloads") {
    // `asset-import-pipeline`: "WHEN the same source and options are imported twice THEN the cooked
    // output SHALL be byte-identical." This is the property the content-addressed cook cache rests
    // on, and it is asserted on the bytes rather than on a hash of them so a failure says which
    // sub-asset moved.
    const std::string document = quad_document("Panel", 2, 1.0, true);
    const ImportResult first = import_document(document, nullptr);
    const ImportResult second = import_document(document, nullptr);

    CY_REQUIRE(first.assets().size() == second.assets().size());
    for (usize index = 0; index < first.assets().size(); ++index) {
        const SubAsset& left = first.assets()[index];
        const SubAsset& right = second.assets()[index];
        CY_CHECK(left.view() == right.view());
        CY_REQUIRE(left.payload.size() == right.payload.size());
        for (usize at = 0; at < left.payload.size(); ++at) {
            CY_REQUIRE(left.payload[at] == right.payload[at]);
        }
    }
}

CY_TEST_CASE("fbx: bytes that are not an FBX are a diagnostic, not a crash and not an error") {
    FbxImporter importer;
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise("models/broken.fbx").value();
    static constexpr char kRubbish[] = "this is not an FBX file, it is a sentence";
    request.bytes = cy::Span<const u8>(reinterpret_cast<const u8*>(kRubbish), sizeof(kRubbish) - 1);
    ImportResult result;
    // A failure that is about the SOURCE is a diagnostic on the result; a returned error would mean
    // the importer itself is broken, and the pipeline treats the two differently.
    CY_CHECK(importer.import(request, result).has_value());
    CY_CHECK(result.has_errors());
    CY_CHECK(result.assets().empty());
}

CY_TEST_CASE("fbx: a scene with no meshes still produces a prefab") {
    // An FBX exported as a layout — nodes and no geometry — is a legitimate asset, and an importer
    // that produced nothing for it would leave the project with a source file and no asset.
    std::string text = quad_document("Panel", 1, 100.0, false);
    ImportOptions options;
    const OptionsSchema schema = fbx_options();
    CY_REQUIRE(options.set(schema, "import-meshes", OptionValue::of_bool(false)).has_value());
    const ImportResult result = import_document(text, &options);
    CY_CHECK(find(result, "mesh/quad") == nullptr);
    CY_REQUIRE(find(result, "prefab") != nullptr);
    CY_CHECK(find(result, "prefab")->primary);
}
