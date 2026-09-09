// Generated primitives, and the assertion that nothing downstream can tell one from an import.
// M8.a tasks 2.1, 2.3 and 2.4.
//
// THE CASE THIS SUITE EXISTS FOR is "a generated box and an imported box cook to the same bytes".
// design.md §2 says a created box must be "a mesh instance whose mesh the engine generated rather
// than imported — the same components, the same asset handles, the same cooking path", and the only
// way to assert that rather than assert around it is to produce the same geometry twice, once
// through `PrimitiveImporter` and once through `GltfImporter`, and compare the cooked payload byte
// for byte. That case is below and it is the one to fix first if it goes red.
//
// The rest of the suite is what makes the generators worth trusting: each shape is closed, wound
// counter-clockwise, and sits where its origin says it does; the source format refuses what it
// cannot honour; and the derivation key is the importers' own, so a changed parameter re-cooks and
// an unchanged one does not.
//
// Integration rather than unit for the reason tools/import/tests/CMakeLists.txt already gives about
// `import_gltf`: welding, tangent generation and the vertex-cache reorder over a 512-triangle
// sphere are over the unit taxonomy's millisecond in Debug, and the last case writes a project to a
// scratch directory and cooks it twice.

#include <cy/core/assets/derivation.h>
#include <cy/core/assets/derived_cache.h>
#include <cy/core/assets/file.h>
#include <cy/core/math/scalar.h>
#include <cy/import/gltf.h>
#include <cy/import/pipeline.h>
#include <cy/import/primitive.h>
#include <cy/test/test.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cy::import;
using cy::f32;
using cy::u32;
using cy::u8;
using cy::usize;
using cy::Vec3;

namespace {

// --- The golden source text
// -----------------------------------------------------------------------
//
// THIS EXACT STRING IS PINNED ON BOTH SIDES OF THE PROCESS BOUNDARY. The editor writes a `.cyprim`
// in Rust and this build parses it in C++; the source bytes are hashed into the derivation key, so
// two spellings of one primitive are two cooked artefacts of one primitive. The Rust suite
// `editor/crates/cy-editor-services/tests/primitives_are_ordinary_mesh_instances.rs` asserts its
// writer produces these bytes, and this asserts the C++ writer produces them and the C++ parser
// reads them back. A change to either spelling turns the other red.
constexpr std::string_view kGoldenBox =
    "cyprim 1\n"
    "shape box\n"
    "name Crate\n"
    "origin base\n"
    "extent 1 2 0.5\n";

// --- Helpers
// ----------------------------------------------------------------------------------------

ImportResult import_source(const Importer& importer, std::string_view path, std::string_view text,
                           const ImportOptions* options) {
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise(path).value();
    request.bytes = cy::Span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size());
    request.options = options;
    ImportResult result;
    CY_REQUIRE(const_cast<Importer&>(importer).import(request, result).has_value());
    return result;
}

ImportResult import_primitive(std::string_view text, const ImportOptions* options = nullptr) {
    PrimitiveImporter importer;
    return import_source(importer, "shapes/thing.cyprim", text, options);
}

const SubAsset* find(const ImportResult& result, std::string_view name) {
    for (const SubAsset& produced : result.assets()) {
        if (produced.view() == name) {
            return &produced;
        }
    }
    return nullptr;
}

MeshData cooked_mesh_of(const SubAsset& produced) {
    MeshData mesh;
    CY_REQUIRE(
        read_cooked_mesh(cy::Span<const u8>(produced.payload.data(), produced.payload.size()), mesh)
            .has_value());
    return mesh;
}

PrimitiveSpec spec_of(std::string_view text) {
    auto parsed = parse_primitive_source(text);
    CY_REQUIRE(parsed.has_value());
    return std::move(parsed.value());
}

MeshData generated(std::string_view text) {
    MeshData mesh;
    CY_REQUIRE(build_primitive_mesh(spec_of(text), mesh).has_value());
    return mesh;
}

/// A source naming one shape and nothing else, which is the smallest complete `.cyprim`.
std::string bare(std::string_view shape) {
    return std::string("cyprim 1\nshape ") + std::string(shape) + "\n";
}

/// Every triangle's geometric normal agrees with its vertices' shading normals.
///
/// This is the winding assertion, and it is a stronger one than checking a single face: a shape
/// wound the wrong way round has every triangle's cross product pointing into the volume, and a
/// shape wound inconsistently has some of them. Either fails here.
bool wound_outwards(const MeshData& mesh) {
    for (usize triangle = 0; triangle < mesh.triangle_count(); ++triangle) {
        const u32 a = mesh.indices[(triangle * 3) + 0];
        const u32 b = mesh.indices[(triangle * 3) + 1];
        const u32 c = mesh.indices[(triangle * 3) + 2];
        const Vec3 face =
            cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
        for (const u32 corner : {a, b, c}) {
            if (dot(face, mesh.normals[corner]) <= 0.0f) {
                return false;
            }
        }
    }
    return true;
}

/// A closed surface: every undirected edge is shared by exactly two triangles.
///
/// Run on the WELDED mesh, because a generated ring duplicates its seam on purpose and an unwelded
/// sphere is therefore open along one meridian by construction.
bool closed(const MeshData& mesh) {
    std::map<std::pair<u32, u32>, u32> edges;
    for (usize triangle = 0; triangle < mesh.triangle_count(); ++triangle) {
        for (usize corner = 0; corner < 3; ++corner) {
            const u32 from = mesh.indices[(triangle * 3) + corner];
            const u32 to = mesh.indices[(triangle * 3) + ((corner + 1) % 3)];
            edges[{from < to ? from : to, from < to ? to : from}] += 1;
        }
    }
    for (const auto& [edge, count] : edges) {
        if (count != 2) {
            return false;
        }
    }
    return !edges.empty();
}

/// Weld a generated mesh the way the importer's first shared step does, so that `closed` is asked
/// about the surface rather than about the seam.
MeshData welded(std::string_view text) {
    MeshData mesh = generated(text);
    WeldOptions options;
    // Position only: the seam and the hard edges are exactly the vertices whose normals or UVs
    // differ, and this asks about the geometry underneath them.
    options.normal_tolerance_degrees = 180.0f;
    options.uv_tolerance = 1.0e9f;
    CY_REQUIRE(weld(mesh, options).has_value());
    return mesh;
}

// --- A glTF carrying an arbitrary mesh
// --------------------------------------------------------------
//
// Built here rather than checked in, for the reason test_gltf.cpp gives: a fixture is a binary
// nobody can read in a review. This one is built FROM a generated primitive, which is what lets the
// comparison case below assert that two producers of the same geometry cook the same bytes.

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

void append_u32(std::vector<u8>& bytes, u32 value) {
    for (u32 index = 0; index < 4; ++index) {
        bytes.push_back(static_cast<u8>((value >> (index * 8U)) & 0xFFU));
    }
}

/// A glTF whose single mesh holds `mesh`'s positions, normals, texture coordinates and indices,
/// bit for bit, under one node with the given name.
std::string gltf_of(const MeshData& mesh, std::string_view node_name) {
    std::vector<u8> buffer;
    const usize positions_offset = buffer.size();
    for (const Vec3& position : mesh.positions) {
        append_f32(buffer, position.x);
        append_f32(buffer, position.y);
        append_f32(buffer, position.z);
    }
    const usize normals_offset = buffer.size();
    for (const Vec3& normal : mesh.normals) {
        append_f32(buffer, normal.x);
        append_f32(buffer, normal.y);
        append_f32(buffer, normal.z);
    }
    const usize uvs_offset = buffer.size();
    for (const cy::Vec2& uv : mesh.uvs) {
        append_f32(buffer, uv.x);
        append_f32(buffer, uv.y);
    }
    const usize indices_offset = buffer.size();
    for (const u32 index : mesh.indices) {
        append_u32(buffer, index);
    }

    const auto view = [](usize offset, usize length) {
        char text[128] = {};
        (void)std::snprintf(text, sizeof(text), R"({"buffer":0,"byteOffset":%zu,"byteLength":%zu})",
                            offset, length);
        return std::string(text);
    };
    const auto accessor = [](usize view_index, const char* type, u32 component, usize count) {
        char text[192] = {};
        (void)std::snprintf(text, sizeof(text),
                            R"({"bufferView":%zu,"componentType":%u,"count":%zu,"type":"%s"})",
                            view_index, component, count, type);
        return std::string(text);
    };

    const usize vertices = mesh.vertex_count();
    std::string document =
        R"({"asset":{"version":"2.0"},"buffers":[{"uri":"data:application/octet-stream;base64,)";
    document += base64(buffer);
    document += R"(","byteLength":)" + std::to_string(buffer.size()) + "}],";
    document += R"("bufferViews":[)" + view(positions_offset, vertices * 12) + "," +
                view(normals_offset, vertices * 12) + "," + view(uvs_offset, vertices * 8) + "," +
                view(indices_offset, mesh.indices.size() * 4) + "],";
    document += R"("accessors":[)" + accessor(0, "VEC3", 5126, vertices) + "," +
                accessor(1, "VEC3", 5126, vertices) + "," + accessor(2, "VEC2", 5126, vertices) +
                "," + accessor(3, "SCALAR", 5125, mesh.indices.size()) + "],";
    document +=
        R"("meshes":[{"name":"Crate","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3}]}],)";
    document += R"("nodes":[{"name":")";
    document += node_name;
    document += R"(","mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    return document;
}

// --- A project on disk, for the cache cases
// ---------------------------------------------------------

class TempDir {
public:
    explicit TempDir(const char* label) {
        static std::atomic<unsigned> serial{0};
        char buffer[256] = {};
        (void)std::snprintf(buffer, sizeof(buffer), "cy_primitive_test_%s_%u", label,
                            serial.fetch_add(1));
        path_ = buffer;
        (void)cy::assets::fs::remove_directory_recursive(path_.c_str());
        CY_REQUIRE(cy::assets::fs::create_directories(path_.c_str()).has_value());
    }

    ~TempDir() { (void)cy::assets::fs::remove_directory_recursive(path_.c_str()); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] std::string file(const char* name) const { return path_ + "/" + name; }

private:
    std::string path_;
};

void write_text(const std::string& path, std::string_view text) {
    const std::string::size_type slash = path.rfind('/');
    if (slash != std::string::npos) {
        CY_REQUIRE(cy::assets::fs::create_directories(path.substr(0, slash).c_str()).has_value());
    }
    CY_REQUIRE(cy::assets::fs::write_atomic(path.c_str(), text.data(), text.size()).has_value());
}

}  // namespace

// --- The source format
// ----------------------------------------------------------------------------

CY_TEST_CASE("primitive: the canonical text round-trips, and it is the text the editor writes") {
    const PrimitiveSpec spec = spec_of(kGoldenBox);
    CY_CHECK(spec.shape == PrimitiveShape::Box);
    CY_CHECK(spec.name == "Crate");
    CY_CHECK(spec.origin == PrimitiveOrigin::Base);
    CY_CHECK(spec.parameters.extent.y == 2.0f);
    CY_CHECK(spec.parameters.extent.z == 0.5f);

    std::string written;
    CY_REQUIRE(write_primitive_source(spec, written).has_value());
    CY_CHECK(written == kGoldenBox);
}

CY_TEST_CASE("primitive: a shape on its own is a complete source, and takes the shape's own name") {
    for (const auto& [shape, label] : {std::pair<std::string_view, std::string_view>{"box", "Box"},
                                       {"sphere", "Sphere"},
                                       {"cylinder", "Cylinder"},
                                       {"plane", "Plane"},
                                       {"capsule", "Capsule"}}) {
        const PrimitiveSpec spec = spec_of(bare(shape));
        CY_CHECK(spec.name == label);
        CY_CHECK(spec.origin == PrimitiveOrigin::Centre);
    }
}

CY_TEST_CASE("primitive: a parameter the shape does not take is refused, naming its line") {
    // A `radius` on a box is the defect this refusal exists for: accepted and ignored, it would sit
    // in a source file changing the derivation key and nothing else, and a designer would edit it
    // for an afternoon.
    cy::u32 line = 0;
    auto parsed = parse_primitive_source("cyprim 1\nshape box\nradius 3\n", &line);
    CY_CHECK(!parsed.has_value());
    CY_CHECK(line == 3);

    auto unknown = parse_primitive_source("cyprim 1\nshape box\nbevel 3\n", &line);
    CY_CHECK(!unknown.has_value());
    CY_CHECK(line == 3);

    auto twice = parse_primitive_source("cyprim 1\nshape box\nextent 1 1 1\nextent 2 2 2\n", &line);
    CY_CHECK(!twice.has_value());
    CY_CHECK(line == 4);

    auto early = parse_primitive_source("cyprim 1\nextent 1 1 1\nshape box\n", &line);
    CY_CHECK(!early.has_value());
    CY_CHECK(line == 2);

    CY_CHECK(!parse_primitive_source("cyprim 2\nshape box\n").has_value());
    CY_CHECK(!parse_primitive_source("shape box\n").has_value());
    CY_CHECK(!parse_primitive_source("").has_value());
    // Out of range, both ends.
    CY_CHECK(!parse_primitive_source("cyprim 1\nshape sphere\nsegments 2\n").has_value());
    CY_CHECK(!parse_primitive_source("cyprim 1\nshape sphere\nsegments 100000\n").has_value());
    CY_CHECK(!parse_primitive_source("cyprim 1\nshape sphere\nradius 0\n").has_value());
}

CY_TEST_CASE("primitive: comments and blank lines are ignored") {
    const PrimitiveSpec spec = spec_of(
        "# a crate\ncyprim 1\n\n  shape box  # the shape\nextent 2 2 2\n\n# and nothing else\n");
    CY_CHECK(spec.shape == PrimitiveShape::Box);
    CY_CHECK(spec.parameters.extent.x == 2.0f);
}

// --- The geometry
// ------------------------------------------------------------------------------------

CY_TEST_CASE("primitive: every shape generates a valid mesh with normals and coordinates") {
    for (const std::string_view shape : {"box", "sphere", "cylinder", "plane", "capsule"}) {
        const MeshData mesh = generated(bare(shape));
        CY_CHECK(mesh.validate().has_value());
        CY_CHECK(mesh.vertex_count() > 0);
        CY_CHECK(mesh.triangle_count() > 0);
        CY_CHECK(mesh.normals.size() == mesh.vertex_count());
        CY_CHECK(mesh.uvs.size() == mesh.vertex_count());
        CY_CHECK(mesh.sections.size() == 1);
        CY_CHECK(mesh.sections[0].index_count == mesh.indices.size());
        // Tangents are `finish_mesh`'s job and nobody else's — see primitive.h.
        CY_CHECK(mesh.tangents.empty());
        CY_CHECK(wound_outwards(mesh));
    }
}

CY_TEST_CASE("primitive: the closed shapes are closed") {
    // A plane is deliberately absent: it is a sheet, and an edge shared by one triangle is what a
    // sheet is.
    for (const std::string_view shape : {"box", "sphere", "cylinder", "capsule"}) {
        CY_CHECK(closed(welded(bare(shape))));
    }
}

CY_TEST_CASE("primitive: a shape's bounds are the size its parameters asked for") {
    const cy::Aabb box = generated("cyprim 1\nshape box\nextent 2 4 6\n").bounds();
    CY_CHECK(cy::math::nearly_equal(box.min.x, -1.0f));
    CY_CHECK(cy::math::nearly_equal(box.max.y, 2.0f));
    CY_CHECK(cy::math::nearly_equal(box.max.z, 3.0f));

    const cy::Aabb sphere = generated("cyprim 1\nshape sphere\nradius 2\n").bounds();
    CY_CHECK(cy::math::nearly_equal(sphere.min.y, -2.0f));
    CY_CHECK(cy::math::nearly_equal(sphere.max.y, 2.0f));

    // A capsule's `height` is its cylindrical section, so its total height is height + 2r. That is
    // the physics convention, and a capsule that disagreed with its own collider would be the first
    // thing a body on it found.
    const cy::Aabb capsule = generated("cyprim 1\nshape capsule\nradius 0.5\nheight 2\n").bounds();
    CY_CHECK(cy::math::nearly_equal(capsule.max.y, 1.5f));
    CY_CHECK(cy::math::nearly_equal(capsule.min.y, -1.5f));

    const cy::Aabb plane = generated("cyprim 1\nshape plane\nextent 10 4\n").bounds();
    CY_CHECK(cy::math::nearly_equal(plane.min.x, -5.0f));
    CY_CHECK(cy::math::nearly_equal(plane.max.z, 2.0f));
    CY_CHECK(cy::math::nearly_equal(plane.max.y, 0.0f));
}

CY_TEST_CASE("primitive: origin base puts the lowest point on the floor") {
    for (const std::string_view shape : {"box", "sphere", "cylinder", "capsule"}) {
        const std::string source = bare(shape) + "origin base\n";
        const cy::Aabb bounds = generated(source).bounds();
        CY_CHECK(cy::math::nearly_equal(bounds.min.y, 0.0f));
        CY_CHECK(bounds.max.y > 0.0f);
    }
}

CY_TEST_CASE("primitive: subdivisions and segments change the tessellation and nothing else") {
    const MeshData coarse = generated("cyprim 1\nshape plane\nsubdivisions 1 1\n");
    const MeshData fine = generated("cyprim 1\nshape plane\nsubdivisions 4 4\n");
    CY_CHECK(coarse.triangle_count() == 2);
    CY_CHECK(fine.triangle_count() == 32);
    CY_CHECK(cy::math::nearly_equal(coarse.bounds().max.x, fine.bounds().max.x));

    const MeshData rough = generated("cyprim 1\nshape sphere\nsegments 8\nrings 4\n");
    const MeshData smooth = generated("cyprim 1\nshape sphere\nsegments 64\nrings 32\n");
    CY_CHECK(rough.triangle_count() < smooth.triangle_count());
    CY_CHECK(wound_outwards(smooth));
}

// --- The importer
// -------------------------------------------------------------------------------------

CY_TEST_CASE("primitive: an import produces a mesh and a prefab, and exactly one primary") {
    const ImportResult result = import_primitive(kGoldenBox);
    CY_CHECK(!result.has_errors());

    const SubAsset* mesh = find(result, "mesh/Crate");
    CY_REQUIRE(mesh != nullptr);
    CY_CHECK(mesh->kind == cy::assets::AssetKind::Mesh);

    const SubAsset* prefab = find(result, "prefab");
    CY_REQUIRE(prefab != nullptr);
    CY_CHECK(prefab->primary);
    CY_CHECK(prefab->kind == cy::assets::AssetKind::Prefab);

    usize primaries = 0;
    for (const SubAsset& produced : result.assets()) {
        primaries += produced.primary ? 1U : 0U;
    }
    CY_CHECK(primaries == 1);

    // The cooked mesh carries the tangents `finish_mesh` generated, which is the same statement
    // test_gltf.cpp makes about an imported one.
    const MeshData cooked = cooked_mesh_of(*mesh);
    CY_CHECK(cooked.tangents.size() == cooked.vertex_count());
    CY_CHECK(cooked.normals.size() == cooked.vertex_count());
}

CY_TEST_CASE("primitive: a malformed source is a diagnostic, not a returned error") {
    const ImportResult result = import_primitive("cyprim 1\nshape dodecahedron\n");
    CY_CHECK(result.has_errors());
    CY_CHECK(result.assets().empty());
    CY_REQUIRE(!result.diagnostics().empty());
    CY_CHECK(std::string_view(result.diagnostics()[0].code) == "malformed-primitive");
    // The subject names the file and the line, which is the half of the diagnostic a person acts
    // on.
    CY_CHECK(std::string_view(result.diagnostics()[0].subject).find(":2") !=
             std::string_view::npos);
}

CY_TEST_CASE("primitive: importing twice produces byte-identical output") {
    // `asset-import-pipeline` requires byte-identical output for identical input, and a generator
    // is the easiest place in a pipeline to break it — an unordered container, an address-ordered
    // tie-break, an accumulated float.
    const ImportResult first = import_primitive(bare("sphere"));
    const ImportResult second = import_primitive(bare("sphere"));
    CY_REQUIRE(first.assets().size() == second.assets().size());
    for (usize index = 0; index < first.assets().size(); ++index) {
        const SubAsset& a = first.assets()[index];
        const SubAsset& b = second.assets()[index];
        CY_CHECK(a.view() == b.view());
        CY_REQUIRE(a.payload.size() == b.payload.size());
        CY_CHECK(std::memcmp(a.payload.data(), b.payload.data(), a.payload.size()) == 0);
    }
}

CY_TEST_CASE("primitive: a name ending in the collision suffix is a collider and is not drawn") {
    // The same convention, the same option and the same behaviour as a model import's node — see
    // primitive.h on why this is not a parameter of its own.
    const ImportResult result = import_primitive("cyprim 1\nshape box\nname Crate_collision\n");
    CY_CHECK(!result.has_errors());
    CY_CHECK(find(result, "collision/Crate_collision") != nullptr);

    const SubAsset* prefab = find(result, "prefab");
    CY_REQUIRE(prefab != nullptr);
    cy::Array<ImportedNode> nodes;
    cy::Array<char> names;
    CY_REQUIRE(read_cooked_scene_graph(
                   cy::Span<const u8>(prefab->payload.data(), prefab->payload.size()), nodes, names)
                   .has_value());
    CY_REQUIRE(nodes.size() == 1);
    CY_CHECK(nodes[0].collision_only);
    CY_CHECK(nodes[0].mesh == -1);
    CY_CHECK(nodes[0].collision == 0);
}

CY_TEST_CASE("primitive: an ordinary primitive is one drawn node with no collider") {
    const ImportResult result = import_primitive(kGoldenBox);
    const SubAsset* prefab = find(result, "prefab");
    CY_REQUIRE(prefab != nullptr);
    cy::Array<ImportedNode> nodes;
    cy::Array<char> names;
    CY_REQUIRE(read_cooked_scene_graph(
                   cy::Span<const u8>(prefab->payload.data(), prefab->payload.size()), nodes, names)
                   .has_value());
    CY_REQUIRE(nodes.size() == 1);
    CY_CHECK(std::string_view(nodes[0].name) == "Crate");
    CY_CHECK(nodes[0].parent == -1);
    CY_CHECK(nodes[0].mesh == 0);
    CY_CHECK(nodes[0].collision == -1);
    CY_CHECK(!nodes[0].collision_only);
    CY_CHECK(find(result, "collision/Crate") == nullptr);
}

CY_TEST_CASE("primitive: a level-of-detail chain is generated to the configured depth") {
    ImportOptions options;
    const OptionsSchema schema = primitive_options();
    CY_REQUIRE(options.set(schema, "lod-count", OptionValue::of_int(2)).has_value());
    const ImportResult result = import_primitive(bare("sphere"), &options);
    CY_CHECK(find(result, "mesh/Sphere") != nullptr);
    CY_CHECK(find(result, "mesh/Sphere/lod1") != nullptr);
    CY_CHECK(find(result, "mesh/Sphere/lod2") != nullptr);
    CY_CHECK(find(result, "mesh/Sphere/lod3") == nullptr);
}

// --- 2.3: nothing downstream can tell it from an import
// ----------------------------------------------

CY_TEST_CASE("primitive: a generated box and an imported box cook to the same bytes") {
    // THE HEADLINE CASE. The same geometry reaches `finish_mesh` from two producers, and what comes
    // out the other side is compared byte for byte. Anything a generator did of its own — its own
    // welding, its own tangents, its own vertex order, its own payload writer — makes this fail.
    const MeshData shape = generated("cyprim 1\nshape box\nname Crate\n");
    const std::string document = gltf_of(shape, "Crate");

    GltfImporter gltf;
    const ImportResult imported = import_source(gltf, "models/crate.gltf", document, nullptr);
    const ImportResult made = import_primitive("cyprim 1\nshape box\nname Crate\n");
    CY_CHECK(!imported.has_errors());
    CY_CHECK(!made.has_errors());

    const SubAsset* from_file = find(imported, "mesh/Crate");
    const SubAsset* from_parameters = find(made, "mesh/Crate");
    CY_REQUIRE(from_file != nullptr);
    CY_REQUIRE(from_parameters != nullptr);
    CY_CHECK(from_file->kind == from_parameters->kind);
    CY_REQUIRE(from_file->payload.size() == from_parameters->payload.size());
    CY_CHECK(std::memcmp(from_file->payload.data(), from_parameters->payload.data(),
                         from_file->payload.size()) == 0);

    // And the hierarchy each produced is the same one-node prefab, so a consumer reading the
    // primary sub-asset sees no difference either.
    cy::Array<ImportedNode> imported_nodes;
    cy::Array<ImportedNode> made_nodes;
    cy::Array<char> imported_names;
    cy::Array<char> made_names;
    const SubAsset* imported_prefab = find(imported, "prefab");
    const SubAsset* made_prefab = find(made, "prefab");
    CY_REQUIRE(imported_prefab != nullptr);
    CY_REQUIRE(made_prefab != nullptr);
    CY_REQUIRE(read_cooked_scene_graph(cy::Span<const u8>(imported_prefab->payload.data(),
                                                          imported_prefab->payload.size()),
                                       imported_nodes, imported_names)
                   .has_value());
    CY_REQUIRE(read_cooked_scene_graph(
                   cy::Span<const u8>(made_prefab->payload.data(), made_prefab->payload.size()),
                   made_nodes, made_names)
                   .has_value());
    CY_REQUIRE(imported_nodes.size() == 1);
    CY_REQUIRE(made_nodes.size() == 1);
    CY_CHECK(std::string_view(imported_nodes[0].name) == std::string_view(made_nodes[0].name));
    CY_CHECK(imported_nodes[0].mesh == made_nodes[0].mesh);
    CY_CHECK(imported_nodes[0].collision == made_nodes[0].collision);
    CY_CHECK(imported_prefab->primary == made_prefab->primary);
}

CY_TEST_CASE("primitive: a generated collider and an imported one cook to the same bytes") {
    // THE HALF OF TASK 2.3 THAT PHYSICS WILL READ. `src/physics/` does not exist yet — it is task
    // 4.1 of this same milestone — so "physics cannot tell them apart" cannot be asserted against a
    // body today. What CAN be asserted is the thing a body will be built from: the cooked collision
    // sub-asset. It is produced by the same `emit_collision`, from the source mesh and never from a
    // level of detail, under the same `collision-suffix` convention, and it comes out byte for byte
    // the same from both producers.
    const MeshData shape = generated("cyprim 1\nshape box\nname Crate_collision\n");
    const std::string document = gltf_of(shape, "Crate_collision");

    GltfImporter gltf;
    const ImportResult imported = import_source(gltf, "models/crate.gltf", document, nullptr);
    const ImportResult made = import_primitive("cyprim 1\nshape box\nname Crate_collision\n");

    const SubAsset* from_file = find(imported, "collision/Crate_collision");
    const SubAsset* from_parameters = find(made, "collision/Crate_collision");
    CY_REQUIRE(from_file != nullptr);
    CY_REQUIRE(from_parameters != nullptr);
    CY_CHECK(from_file->kind == from_parameters->kind);
    CY_REQUIRE(from_file->payload.size() == from_parameters->payload.size());
    CY_CHECK(std::memcmp(from_file->payload.data(), from_parameters->payload.data(),
                         from_file->payload.size()) == 0);
}

CY_TEST_CASE("primitive: the importer declares itself like every other one") {
    // `asset-import-pipeline`'s framework requirement, and the reason a primitive needs no
    // privilege: it is found by extension in the same registry, and a caller that lists importers
    // sees a sentence it can act on.
    ImporterRegistry registry;
    CY_REQUIRE(register_builtin_importers(registry).has_value());
    Importer* found = registry.find_for_extension(".cyprim");
    CY_REQUIRE(found != nullptr);
    CY_CHECK(found->info().name == "primitive");
    CY_CHECK(!found->info().description.empty());
    CY_CHECK(found->schema().validate().has_value());

    const auto source = cy::assets::VirtualPath::normalise("shapes/crate.cyprim");
    CY_REQUIRE(source.has_value());
    CY_CHECK(registry.find_for_source(source.value()) == found);
}

// --- 2.4: one derivation key, one cache
// ----------------------------------------------------------------

CY_TEST_CASE("primitive: the derivation key is the importers' own") {
    PrimitiveImporter importer;
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise("shapes/crate.cyprim").value();
    request.variant = cy::assets::VariantKey::parse("desktop-bc7").value();

    const cy::assets::ContentHash one =
        cy::assets::content_hash(kGoldenBox.data(), kGoldenBox.size());
    const std::string other(bare("sphere"));
    const cy::assets::ContentHash two = cy::assets::content_hash(other.data(), other.size());

    auto key_of = [&](const cy::assets::ContentHash& hash) {
        auto key = import_derivation_key(importer.info(), importer.schema(), request, hash);
        CY_REQUIRE(key.has_value());
        return key.value();
    };
    CY_CHECK(key_of(one) == key_of(one));
    CY_CHECK(key_of(one) != key_of(two));

    // The producer is in the key, so a `.cyprim` and a `.gltf` of identical bytes could never
    // collide in the one cache they share.
    GltfImporter gltf;
    auto mine = import_derivation_key(importer.info(), importer.schema(), request, one);
    auto theirs = import_derivation_key(gltf.info(), gltf.schema(), request, one);
    CY_REQUIRE(mine.has_value());
    CY_REQUIRE(theirs.has_value());
    CY_CHECK(mine.value() != theirs.value());
}

CY_TEST_CASE("primitive: the second cook is a cache hit and a changed parameter is not") {
    // The property 2.4 is actually about: a generated mesh is "cacheable and deterministic like any
    // other derived artefact", through the pipeline every other source goes through, with no second
    // cache and no second key.
    TempDir directory("cache");
    const std::string project = directory.file("project");
    const std::string output = directory.file("cooked");
    const std::string cache_root = directory.file("cache");
    const std::string source = project + "/shapes/crate.cyprim";

    ImporterRegistry registry;
    CY_REQUIRE(register_builtin_importers(registry).has_value());
    cy::assets::DerivedCache cache;
    cy::assets::DerivedCacheTiers tiers;
    tiers.local = cache_root.c_str();
    CY_REQUIRE(cache.configure(tiers).has_value());
    CY_REQUIRE(cy::assets::fs::create_directories(project.c_str()).has_value());

    const auto path = cy::assets::VirtualPath::normalise("shapes/crate.cyprim").value();
    const auto cook = [&](cy::assets::CacheOutcome expected) {
        ImportPipeline pipeline(registry, cache);
        CY_REQUIRE(pipeline.set_project_root(project).has_value());
        CY_REQUIRE(pipeline.set_output_directory(output).has_value());
        ImportSettings settings;
        auto outcome = pipeline.import_file(path, settings);
        CY_REQUIRE(outcome.has_value());
        CY_CHECK(outcome.value().succeeded());
        CY_CHECK(outcome.value().cache == expected);
        CY_CHECK(outcome.value().sub_assets >= 2);
    };

    write_text(source, "cyprim 1\nshape box\nname Crate\nextent 1 1 1\n");
    cook(cy::assets::CacheOutcome::Miss);
    cook(cy::assets::CacheOutcome::Hit);

    write_text(source, "cyprim 1\nshape box\nname Crate\nextent 2 1 1\n");
    cook(cy::assets::CacheOutcome::Miss);
}
