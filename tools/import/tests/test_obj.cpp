// Wavefront OBJ import, end to end in memory. M8.a tasks 3.2, 3.3 and 3.4.
//
// The documents are built here rather than checked in as fixtures, for the reason `test_gltf.cpp`
// gives: a fixture is a file somebody has to maintain and nobody can read in a review. An OBJ is
// text, so this costs nothing and every case can be read beside its assertion.
//
// WHAT IS ASSERTED, AND WHY EACH IS HERE RATHER THAN TRUSTED:
//
//   * the schema matches BOTH other model importers', name for name — the three lists are edited in
//     three files and nothing but this stops them drifting apart;
//   * the two parsing rules that corrupt a mesh SILENTLY when they are got wrong: negative indices
//     relative to the end, and per-corner de-indexing brought back by the shared weld;
//   * the steps the format does not reach are DECLARED and none of them is a warning, which is the
//     requirement's own distinction and the whole of task 3.3;
//   * one derivation key: the key is `import_derivation_key`'s, it carries the toolchain, and it
//     differs between the three importers — which is task 3.4 and the M6 defect it names.
//
// Integration rather than unit for the reason `import_gltf` is: a mesh is welded, tangent-generated
// and run through three optimisers, and the determinism case imports the same file twice to compare
// every byte.

#include <cy/core/math/scalar.h>
#include <cy/import/fbx.h>
#include <cy/import/gltf.h>
#include <cy/import/model.h>
#include <cy/import/obj.h>
#include <cy/import/report.h>
#include <cy/test/test.h>

#include <string>
#include <string_view>
#include <vector>

using namespace cy::import;
using cy::f32;
using cy::u8;
using cy::usize;

namespace {

/// A quad in the XY plane: four positions, four texture coordinates, one normal, two triangles.
///
/// Written the way an exporter writes one — `mtllib`, `o`, `usemtl`, then faces indexing the three
/// arrays separately — because that is the shape the importer has to survive.
constexpr std::string_view kQuad = R"(# a quad
mtllib quad.mtl
o Panel
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
vt 0 0
vt 1 0
vt 1 1
vt 0 1
vn 0 0 1
usemtl Oak
f 1/1/1 2/2/1 3/3/1
f 1/1/1 3/3/1 4/4/1
)";

constexpr std::string_view kQuadMaterial = R"(newmtl Oak
Kd 0.8 0.4 0.2
Ks 0.5 0.5 0.5
Ns 60
d 1
map_Kd oak.tga
)";

/// A resolver that answers from a table and records what was asked for.
///
/// The pipeline's own `FileImportResolver` opens files; these cases have no filesystem, and what
/// they need to assert is that the importer went THROUGH a resolver — which is what files the
/// dependency — rather than that a particular directory was searched.
class TableResolver final : public ImportResolver {
public:
    void add(std::string_view path, std::string_view contents) {
        entries_.push_back({std::string(path), std::string(contents)});
    }

    [[nodiscard]] cy::Expected<cy::Span<const u8>, cy::Error> read(
        std::string_view path) noexcept override {
        asked.emplace_back(path);
        for (const Entry& entry : entries_) {
            if (entry.path == path) {
                return cy::Span<const u8>(reinterpret_cast<const u8*>(entry.contents.data()),
                                          entry.contents.size());
            }
        }
        return cy::fail(cy::ErrorCode::NotFound, "no such file in this table");
    }

    [[nodiscard]] cy::Status observe(std::string_view,
                                     const cy::assets::ContentHash&) noexcept override {
        return cy::ok();
    }

    std::vector<std::string> asked;

private:
    struct Entry {
        std::string path;
        std::string contents;
    };

    std::vector<Entry> entries_;
};

ImportResult import_document(std::string_view text, const ImportOptions* options,
                             TableResolver* resolver) {
    ObjImporter importer;
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise("models/quad.obj").value();
    request.bytes = cy::Span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size());
    request.options = options;
    request.resolver = resolver;
    ImportResult result;
    CY_REQUIRE(importer.import(request, result).has_value());
    return result;
}

ImportResult import_quad() {
    TableResolver resolver;
    resolver.add("quad.mtl", kQuadMaterial);
    resolver.add("oak.tga", "not really a targa, but it is there");
    return import_document(kQuad, nullptr, &resolver);
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

usize diagnostics_of_severity(const ImportResult& result, ImportSeverity severity) {
    usize count = 0;
    for (const ImportDiagnostic& diagnostic : result.diagnostics()) {
        count += diagnostic.severity == severity ? 1U : 0U;
    }
    return count;
}

}  // namespace

CY_TEST_CASE("obj: the importer declares itself and claims .obj") {
    const ObjImporter importer;
    const ImporterInfo info = importer.info();
    CY_CHECK(info.name == "obj");
    CY_REQUIRE(info.extensions.size() == 1);
    CY_CHECK(info.extensions[0] == ".obj");
    CY_CHECK(!info.description.empty());
}

CY_TEST_CASE("obj: the schema matches the glTF and FBX schemas, name for name") {
    // A project that re-exports a model from one format to another keeps its import settings only
    // if the three schemas agree about what an option is called. The three lists are edited in
    // three files; this is what stops them drifting.
    const OptionsSchema obj = obj_options();
    const OptionsSchema gltf = gltf_options();
    const OptionsSchema fbx = fbx_options();
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
        CY_CHECK(obj.find(name) != nullptr);
        CY_CHECK(gltf.find(name) != nullptr);
        CY_CHECK(fbx.find(name) != nullptr);
    }
    // And it declares nothing the other two lack, which is the other half of "the same settings".
    for (const OptionSpec& option : obj.options()) {
        CY_CHECK(gltf.find(option.name) != nullptr);
        CY_CHECK(fbx.find(option.name) != nullptr);
    }
}

CY_TEST_CASE("obj: a quad produces a mesh, a material, and no prefab") {
    const ImportResult result = import_quad();
    CY_CHECK(!result.has_errors());

    const SubAsset* mesh = find(result, "mesh/Panel");
    CY_REQUIRE(mesh != nullptr);
    CY_CHECK(mesh->kind == cy::assets::AssetKind::Mesh);
    CY_CHECK(mesh->primary);

    CY_CHECK(find(result, "material/Oak") != nullptr);

    // Step 10 is not reached, so there is no prefab and nothing pretends there is one. An importer
    // that emitted a flat prefab of identity transforms would be inventing a scene graph the format
    // does not contain — see obj.h.
    CY_CHECK(find(result, "prefab") == nullptr);
    for (const SubAsset& produced : result.assets()) {
        CY_CHECK(produced.kind != cy::assets::AssetKind::Prefab);
    }
}

CY_TEST_CASE("obj: per-corner indexing is undone by the shared weld") {
    const ImportResult result = import_quad();
    const MeshData mesh = mesh_of(*find(result, "mesh/Panel"));

    // OBJ indexes position, texture coordinate and normal separately, so the two triangles arrive
    // as six corners and the weld is what brings them back to four. That is the same welder the
    // glTF and FBX importers run, which is why one model exported to two formats cooks to one
    // vertex count.
    CY_CHECK(mesh.triangle_count() == 2);
    CY_CHECK(mesh.vertex_count() == 4);
    CY_CHECK(mesh.normals.size() == mesh.vertex_count());
    CY_CHECK(mesh.uvs.size() == mesh.vertex_count());
    CY_CHECK(mesh.tangents.size() == mesh.vertex_count());
    CY_REQUIRE(mesh.sections.size() == 1);
    CY_CHECK(mesh.sections[0].index_count == 6);
}

CY_TEST_CASE("obj: a negative index is relative to the end of the array") {
    // The specification's relative form. A reader that treated -1 as an error, or as index 1, still
    // parses the file and produces a mesh — a wrong one — which is why this is asserted rather than
    // trusted.
    constexpr std::string_view kRelative = R"(v 0 0 0
v 1 0 0
v 1 1 0
f -3 -2 -1
)";
    const ImportResult result = import_document(kRelative, nullptr, nullptr);
    CY_CHECK(!result.has_errors());
    const SubAsset* mesh = find(result, "mesh/quad");
    CY_REQUIRE(mesh != nullptr);
    const MeshData built = mesh_of(*mesh);
    CY_CHECK(built.triangle_count() == 1);
    CY_CHECK(built.vertex_count() == 3);
}

CY_TEST_CASE("obj: an n-gon is triangulated by a fan") {
    constexpr std::string_view kPentagon = R"(o Pentagon
v 0 0 0
v 1 0 0
v 1.5 1 0
v 0.5 1.8 0
v -0.5 1 0
f 1 2 3 4 5
)";
    const ImportResult result = import_document(kPentagon, nullptr, nullptr);
    const MeshData mesh = mesh_of(*find(result, "mesh/Pentagon"));
    // Five corners fan to three triangles.
    CY_CHECK(mesh.triangle_count() == 3);
}

CY_TEST_CASE("obj: each usemtl run becomes its own section, in file order") {
    constexpr std::string_view kTwoMaterials = R"(mtllib two.mtl
o Panel
v 0 0 0
v 1 0 0
v 1 1 0
v 0 1 0
usemtl Oak
f 1 2 3
usemtl Steel
f 1 3 4
)";
    constexpr std::string_view kTwoMaterialLibrary = R"(newmtl Oak
Kd 1 0 0
newmtl Steel
Kd 0 0 1
)";
    TableResolver resolver;
    resolver.add("two.mtl", kTwoMaterialLibrary);
    const ImportResult result = import_document(kTwoMaterials, nullptr, &resolver);
    const MeshData mesh = mesh_of(*find(result, "mesh/Panel"));
    CY_REQUIRE(mesh.sections.size() == 2);
    // The material index is the position in the import's own material list, which is the `.mtl`'s
    // declaration order.
    CY_CHECK(mesh.sections[0].material == 0);
    CY_CHECK(mesh.sections[1].material == 1);
    CY_CHECK(mesh.sections[0].index_count == 3);
    CY_CHECK(mesh.sections[1].index_count == 3);
}

CY_TEST_CASE("obj: the .mtl is read through the resolver and mapped onto the standard material") {
    TableResolver resolver;
    resolver.add("quad.mtl", kQuadMaterial);
    resolver.add("oak.tga", "present");
    const ImportResult result = import_document(kQuad, nullptr, &resolver);

    // Reading IS recording: the library and its texture were both asked for through the resolver,
    // which is what files them as dependencies and makes editing a colour re-cook the model.
    bool asked_library = false;
    bool asked_texture = false;
    for (const std::string& path : resolver.asked) {
        asked_library = asked_library || path == "quad.mtl";
        asked_texture = asked_texture || path == "oak.tga";
    }
    CY_CHECK(asked_library);
    CY_CHECK(asked_texture);

    const SubAsset* material = find(result, "material/Oak");
    CY_REQUIRE(material != nullptr);
    StandardMaterial standard;
    CY_REQUIRE(read_cooked_material(
                   cy::Span<const u8>(material->payload.data(), material->payload.size()), standard)
                   .has_value());
    CY_CHECK(cy::math::nearly_equal(standard.base_colour[0], 0.8f, 1e-5f));
    CY_CHECK(cy::math::nearly_equal(standard.base_colour[1], 0.4f, 1e-5f));
    CY_CHECK(cy::math::nearly_equal(standard.base_colour[2], 0.2f, 1e-5f));
    CY_CHECK(cy::math::nearly_equal(standard.base_colour[3], 1.0f, 1e-5f));
    // An `.mtl` is specular-glossiness and the standard material is metallic-roughness: nothing in
    // the format says how metallic a surface is, so a material without the `Pm` extension is
    // dielectric. The alternative default would make every OBJ in the world import as polished
    // metal.
    CY_CHECK(cy::math::nearly_equal(standard.metallic, 0.0f, 1e-5f));
    // Ns 60 through `roughness = sqrt(2 / (Ns + 2))`.
    CY_CHECK(cy::math::nearly_equal(standard.roughness, 0.1796053f, 1e-5f));
    CY_CHECK(standard.alpha_mode == 0);
}

CY_TEST_CASE("obj: the PBR extension wins over the specular exponent") {
    constexpr std::string_view kPbr = R"(newmtl Brass
Kd 0.9 0.7 0.2
Ns 200
Pr 0.35
Pm 1
)";
    const cy::Expected<std::vector<ObjMaterial>, cy::Error> parsed = parse_mtl(kPbr);
    CY_REQUIRE(parsed.has_value());
    CY_REQUIRE(parsed.value().size() == 1);
    CY_CHECK(cy::math::nearly_equal(parsed.value()[0].standard.roughness, 0.35f, 1e-5f));
    CY_CHECK(cy::math::nearly_equal(parsed.value()[0].standard.metallic, 1.0f, 1e-5f));
}

CY_TEST_CASE("obj: the PBR extension wins whichever order it is written in") {
    // An exporter that writes both writes them in either order, and a rule that depended on the
    // order would leave half the world's files importing as mirrors.
    constexpr std::string_view kBefore = R"(newmtl Brass
Pr 0.35
Ns 200
)";
    const cy::Expected<std::vector<ObjMaterial>, cy::Error> parsed = parse_mtl(kBefore);
    CY_REQUIRE(parsed.has_value());
    CY_REQUIRE(parsed.value().size() == 1);
    CY_CHECK(cy::math::nearly_equal(parsed.value()[0].standard.roughness, 0.35f, 1e-5f));
}

CY_TEST_CASE("obj: a dissolve below one produces a blended material") {
    constexpr std::string_view kGlass = R"(newmtl Glass
Kd 0.9 0.9 1
d 0.25
)";
    const cy::Expected<std::vector<ObjMaterial>, cy::Error> parsed = parse_mtl(kGlass);
    CY_REQUIRE(parsed.has_value());
    CY_REQUIRE(parsed.value().size() == 1);
    CY_CHECK(cy::math::nearly_equal(parsed.value()[0].standard.base_colour[3], 0.25f, 1e-5f));
    CY_CHECK(parsed.value()[0].standard.alpha_mode == 2);
}

CY_TEST_CASE("obj: a missing material library is a warning naming it, and the mesh still comes") {
    // A reference to something absent IS a defect in the delivery, so it is a warning — which is a
    // different thing from a step the format cannot express, and the two must not be confused.
    TableResolver resolver;
    const ImportResult result = import_document(kQuad, nullptr, &resolver);
    CY_CHECK(!result.has_errors());
    CY_CHECK(find(result, "mesh/Panel") != nullptr);
    bool named = false;
    for (const ImportDiagnostic& diagnostic : result.diagnostics()) {
        named = named || (std::string_view(diagnostic.code) == "missing-mtl" &&
                          std::string_view(diagnostic.subject) == "quad.mtl");
    }
    CY_CHECK(named);
}

CY_TEST_CASE("obj: a z-up source is converted at import") {
    // `asset-import-pipeline`: "WHEN a Z-up model is imported THEN it SHALL be converted at import
    // so no runtime code accounts for source handedness." OBJ has no hierarchy to put the rotation
    // in, so unlike the glTF and FBX importers this one applies it to the vertices — which is
    // correct precisely because there are no node transforms it could disagree with.
    constexpr std::string_view kZUp = R"(o Post
v 0 0 0
v 1 0 0
v 0 0 2
f 1 2 3
)";
    ImportOptions options;
    const OptionsSchema schema = obj_options();
    CY_REQUIRE(options.set(schema, "source-up-axis", OptionValue::of_enumeration("z-up")));
    const ImportResult result = import_document(kZUp, &options, nullptr);
    const MeshData mesh = mesh_of(*find(result, "mesh/Post"));
    f32 highest = 0.0f;
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        highest = mesh.positions[index].y > highest ? mesh.positions[index].y : highest;
    }
    // Two units along the source's up axis must be two metres along the engine's, which is +Y.
    CY_CHECK(cy::math::nearly_equal(highest, 2.0f, 1e-5f));
}

CY_TEST_CASE("obj: scale converts a file whose author used centimetres") {
    ImportOptions options;
    const OptionsSchema schema = obj_options();
    CY_REQUIRE(options.set(schema, "scale", OptionValue::of_float(0.01)));
    const ImportResult result = import_document(kQuad, &options, nullptr);
    const MeshData mesh = mesh_of(*find(result, "mesh/Panel"));
    f32 widest = 0.0f;
    for (usize index = 0; index < mesh.vertex_count(); ++index) {
        widest = mesh.positions[index].x > widest ? mesh.positions[index].x : widest;
    }
    CY_CHECK(cy::math::nearly_equal(widest, 0.01f, 1e-6f));
}

CY_TEST_CASE("obj: an object named with the collision suffix becomes a collider, not a mesh") {
    // "WHEN a node is named with the configured collision suffix THEN a collider SHALL be generated
    // from it and the node excluded from rendering." OBJ has no nodes, so the name matched is the
    // `o` statement's — the only name the format carries.
    constexpr std::string_view kWithCollider = R"(o Crate
v 0 0 0
v 1 0 0
v 1 1 0
f 1 2 3
o Crate_collision
v 0 0 0
v 2 0 0
v 2 2 0
v 0 0 2
f 4 5 6
f 4 5 7
f 4 6 7
f 5 6 7
)";
    const ImportResult result = import_document(kWithCollider, nullptr, nullptr);
    CY_CHECK(!result.has_errors());
    CY_CHECK(find(result, "mesh/Crate") != nullptr);
    CY_CHECK(find(result, "mesh/Crate_collision") == nullptr);
    CY_CHECK(find(result, "collision/Crate_collision") != nullptr);
    // The primary is the render mesh and never the collider.
    const SubAsset* primary = result.primary();
    CY_REQUIRE(primary != nullptr);
    CY_CHECK(primary->view() == "mesh/Crate");
}

CY_TEST_CASE("obj: the level-of-detail chain is generated to the configured count") {
    constexpr std::string_view kGrid = R"(o Grid
v 0 0 0
v 1 0 0
v 2 0 0
v 0 1 0
v 1 1 0
v 2 1 0
v 0 2 0
v 1 2 0
v 2 2 0
f 1 2 5
f 1 5 4
f 2 3 6
f 2 6 5
f 4 5 8
f 4 8 7
f 5 6 9
f 5 9 8
)";
    ImportOptions options;
    const OptionsSchema schema = obj_options();
    CY_REQUIRE(options.set(schema, "lod-count", OptionValue::of_int(2)));
    const ImportResult result = import_document(kGrid, &options, nullptr);
    CY_CHECK(find(result, "mesh/Grid") != nullptr);
    CY_CHECK(find(result, "mesh/Grid/lod1") != nullptr);
    CY_CHECK(find(result, "mesh/Grid/lod2") != nullptr);
    // A level of detail is never the primary sub-asset.
    CY_CHECK(!find(result, "mesh/Grid/lod1")->primary);
}

CY_TEST_CASE("obj: two imports of one file produce identical bytes") {
    const ImportResult first = import_quad();
    const ImportResult second = import_quad();
    CY_REQUIRE(first.assets().size() == second.assets().size());
    for (usize index = 0; index < first.assets().size(); ++index) {
        CY_CHECK(first.assets()[index].view() == second.assets()[index].view());
        CY_REQUIRE(first.assets()[index].payload.size() == second.assets()[index].payload.size());
        bool identical = true;
        for (usize at = 0; at < first.assets()[index].payload.size(); ++at) {
            identical = identical &&
                        first.assets()[index].payload[at] == second.assets()[index].payload[at];
        }
        CY_CHECK(identical);
    }
}

// --- Task 3.3: the steps it did not reach are NAMED and none of them is a warning
// ------------------

CY_TEST_CASE("obj: the steps the format cannot express are declared, and are 7, 8 and 10") {
    const ObjImporter importer;
    const ModelImportStepSet steps = importer.info().steps;
    CY_CHECK(reaches(steps, ModelImportStep::Parse));
    CY_CHECK(reaches(steps, ModelImportStep::Meshes));
    CY_CHECK(reaches(steps, ModelImportStep::GenerateMissing));
    CY_CHECK(reaches(steps, ModelImportStep::Optimise));
    CY_CHECK(reaches(steps, ModelImportStep::Lods));
    CY_CHECK(reaches(steps, ModelImportStep::Collision));
    CY_CHECK(reaches(steps, ModelImportStep::Materials));
    CY_CHECK(!reaches(steps, ModelImportStep::Skeletons));
    CY_CHECK(!reaches(steps, ModelImportStep::Animations));
    CY_CHECK(!reaches(steps, ModelImportStep::Prefab));

    // Named with their numbers, so a reader can look each one up in the specification's sequence.
    char absent[256] = {};
    const usize written = format_absent_model_steps(steps, absent, sizeof(absent));
    CY_CHECK(written != 0);
    const std::string_view text(absent, written);
    CY_CHECK(text.find("7 (") != std::string_view::npos);
    CY_CHECK(text.find("8 (") != std::string_view::npos);
    CY_CHECK(text.find("10 (") != std::string_view::npos);
    CY_CHECK(text.find("import skeletons") != std::string_view::npos);
    CY_CHECK(text.find("import animations") != std::string_view::npos);
    CY_CHECK(text.find("prefab") != std::string_view::npos);
}

CY_TEST_CASE("obj: a well-formed file with its .mtl beside it raises no warning at all") {
    // The other half of task 3.3, and the one that would go red if an importer reported its absent
    // steps as diagnostics: a clean OBJ with everything it references present must produce ZERO
    // warnings, even though three of the ten steps were not reached.
    const ImportResult result = import_quad();
    CY_CHECK(!result.has_errors());
    CY_CHECK(result.warning_count() == 0);
    CY_CHECK(diagnostics_of_severity(result, ImportSeverity::Warning) == 0);
    for (const ImportDiagnostic& diagnostic : result.diagnostics()) {
        // Nothing may even mention a skeleton or an animation: an OBJ carrying none is not a file
        // with something missing from it.
        CY_CHECK(std::string_view(diagnostic.detail).find("skeleton") == std::string_view::npos);
        CY_CHECK(std::string_view(diagnostic.detail).find("animation") == std::string_view::npos);
    }
}

CY_TEST_CASE("obj: the report names the absent steps under their own heading") {
    // The report is where the requirement says it must appear — "SHALL report the steps it did not
    // reach" — and it must not appear among the warnings.
    ImportReport report;
    AssetImportOutcome row;
    (void)std::snprintf(row.source, sizeof(row.source), "%s", "models/quad.obj");
    (void)std::snprintf(row.importer, sizeof(row.importer), "%s", "obj");
    row.steps = ObjImporter{}.info().steps;
    CY_REQUIRE(report.add(row).has_value());
    CY_CHECK(report.has_absent_steps());
    CY_CHECK(report.total_warnings() == 0);

    char text[4096] = {};
    const usize written = report.format(text, sizeof(text));
    const std::string_view rendered(text, written);
    CY_CHECK(rendered.find("steps not reached") != std::string_view::npos);
    CY_CHECK(rendered.find("not a defect in the file") != std::string_view::npos);
    CY_CHECK(rendered.find("7 (import skeletons)") != std::string_view::npos);
    CY_CHECK(rendered.find("assets with something to report") == std::string_view::npos);
}

CY_TEST_CASE("obj: an importer with no model steps says nothing about them") {
    // A texture importer is not a model importer: it has no steps to have skipped, which is a
    // different statement from having reached none of them.
    ImportReport report;
    AssetImportOutcome row;
    (void)std::snprintf(row.source, sizeof(row.source), "%s", "textures/wall.tga");
    (void)std::snprintf(row.importer, sizeof(row.importer), "%s", "texture");
    row.steps = 0;
    CY_REQUIRE(report.add(row).has_value());
    CY_CHECK(!report.has_absent_steps());
    char text[2048] = {};
    const usize written = report.format(text, sizeof(text));
    CY_CHECK(std::string_view(text, written).find("steps not reached") == std::string_view::npos);
}

// --- Task 3.4: one derivation key, and one cache
// ---------------------------------------------------

namespace {

cy::assets::DerivationKey key_for(const Importer& importer) {
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise("models/thing.bin").value();
    request.variant = cy::assets::VariantKey::parse("desktop-bc7").value();
    request.profile = CookProfile::Client;
    const cy::assets::ContentHash source = cy::assets::content_hash("identical bytes", 15);
    auto key = import_derivation_key(importer.info(), importer.schema(), request, source);
    CY_REQUIRE(key.has_value());
    return key.value();
}

}  // namespace

CY_TEST_CASE("obj: the key is the shared one, and the three formats cannot collide in one cache") {
    // M6's gate measured two importer binaries pointed at ONE cache reporting 1 hit and 0 miss,
    // because the key could not see its own compiler. M7 repaired the function; adding a third
    // format is exactly the moment somebody introduces a second key. There is only one key
    // function, and these are its properties.
    const ObjImporter obj;
    const GltfImporter gltf;
    const FbxImporter fbx;

    // The same bytes under three importers are three entries, never one. If OBJ had built a key of
    // its own that happened to omit the producer, this is the assertion that goes red.
    CY_CHECK(key_for(obj) != key_for(gltf));
    CY_CHECK(key_for(obj) != key_for(fbx));
    CY_CHECK(key_for(gltf) != key_for(fbx));

    // And the same import twice is the same entry, which is what a cache hit rests on.
    CY_CHECK(key_for(obj) == key_for(obj));

    // The toolchain reaches the key. `import_derivation_key` fails on an incomplete fingerprint
    // rather than defaulting, so this is the regression for the M6 defect from the OBJ side.
    CY_CHECK(cy::assets::toolchain_is_complete(cy::assets::current_toolchain()));
}
