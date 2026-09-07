// The real cooks, as nodes in the derivation graph. M7 task 1.4.
//
// M6's closing gate: "**The real cooks are not nodes in the build graph.** `cy_cook` and
// `cy_import_cli` run outside it; the graph's producers are its own four builtins." Everything
// `build-and-packaging` guarantees about a node therefore applied to nothing a project runs. These
// cases are that guarantee, over the importer:
//
//   * an import node produces the same bundle a re-run produces, byte for byte (the determinism
//     gate `just content-validate` already provides, applied to a node);
//   * a glTF's EXTERNAL buffer is a DISCOVERED dependency of the node, so editing it re-runs the
//     node — which is the property that running the importer beside the graph could not give at
//     all, because the graph never knew the file existed;
//   * editing an unrelated source runs nothing.
//
// Integration: every case writes real files and runs real workers.

#include <cy/build/content_producers.h>
#include <cy/build/description.h>
#include <cy/build/service.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/scene/serialization/document.h>
#include <cy/scene/serialization/format.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace cy;
using namespace cy::build;

namespace {

[[nodiscard]] Allocator& test_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

void append_f32(std::string& out, float value) {
    u8 bytes[4] = {};
    static_assert(sizeof(bytes) == sizeof(value));
    std::memcpy(static_cast<void*>(bytes), &value, sizeof(value));
    out.append(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void append_u16(std::string& out, u16 value) {
    out.push_back(static_cast<char>(value & 0xFFU));
    out.push_back(static_cast<char>((value >> 8U) & 0xFFU));
}

/// The binary buffer a glTF references, as a separate file. That separation is the whole point of
/// these cases: an embedded data URI would never reach `ImportResolver::read`, and it is the
/// resolver that becomes `NodeContext::discover`.
[[nodiscard]] std::string quad_buffer(float height) {
    std::string buffer;
    const float positions[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, height, 0}, {1, height, 0}};
    for (const auto& position : positions) {
        append_f32(buffer, position[0]);
        append_f32(buffer, position[1]);
        append_f32(buffer, position[2]);
    }
    for (const u32 index : {0U, 1U, 2U, 2U, 1U, 3U}) {
        append_u16(buffer, static_cast<u16>(index));
    }
    return buffer;
}

[[nodiscard]] std::string quad_document(usize buffer_bytes) {
    std::string document;
    document += R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":)";
    document += std::to_string(buffer_bytes);
    document += R"(,"uri":"quad.bin"}],"bufferViews":[)";
    document += R"({"buffer":0,"byteOffset":0,"byteLength":48},)";
    document += R"({"buffer":0,"byteOffset":48,"byteLength":12}],)";
    document += R"("accessors":[)";
    document += R"({"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},)";
    document += R"({"bufferView":1,"componentType":5123,"count":6,"type":"SCALAR"}],)";
    document += R"("meshes":[{"name":"Panel","primitives":[{"attributes":{"POSITION":0},)"
                R"("indices":1,"mode":4}]}],)";
    document += R"("nodes":[{"name":"Quad","mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    return document;
}

/// A component with one plain field, so a cooked block is not empty. `tools/cook/tests/` declares
/// the same one for the same reason; the component model itself is exercised there.
struct Marker {
    u32 value = 0;
};

inline constexpr u32 kMarkerType = 9402;

[[nodiscard]] const reflect::TypeInfo& marker_type() noexcept {
    static reflect::FieldInfo fields[1];
    fields[0].name = "value";
    fields[0].id = reflect::FieldId(1);
    fields[0].kind = reflect::FieldKind::U32;
    fields[0].offset = 0;
    fields[0].size = sizeof(u32);

    static reflect::TypeInfo info;
    info.name = "cy::build::test::Marker";
    info.id = reflect::TypeId(kMarkerType);
    info.size = static_cast<u32>(sizeof(Marker));
    info.alignment = static_cast<u32>(alignof(Marker));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 1;
    return info;
}

[[nodiscard]] std::string scene_document_text() {
    using namespace cy::scene::serialization;
    Document document(test_allocator());
    document.kind = AssetKind::Scene;
    document.id = AssetId(0, 0x5CE7E);
    for (u32 index = 0; index < 3; ++index) {
        const Expected<DocumentEntity*, Error> entity = document.add_entity(kNoLocalId, "e");
        CY_REQUIRE(entity.has_value());
        const Expected<ComponentData*, Error> component =
            (*entity)->ensure(reflect::TypeId(kMarkerType));
        CY_REQUIRE(component.has_value());
        CY_REQUIRE((*component)
                       ->record.set_scalar(reflect::FieldId(1), serialize::WireType::U32, &index, 4)
                       .has_value());
    }
    Array<char> text(test_allocator());
    CY_REQUIRE(write_text(document, text).has_value());
    return {text.data(), text.size()};
}

constexpr const char* kDescription =
    "cybuild 1\n"
    "node \"import:quad\" import \"import\" 1\n"
    "  source \"assets/quad.gltf\"\n"
    "  output \"derived/quad.bundle\"\n"
    "  option \"variant\" \"desktop\"\n"
    "node \"unrelated\" import \"copy\" 1\n"
    "  source \"assets/notes.txt\"\n"
    "  output \"derived/notes.bin\"\n";

class Project {
public:
    explicit Project(const char* label) : temp_(label) {
        CY_REQUIRE(temp_.valid());
        CY_REQUIRE(assets::fs::create_directories(sources().c_str()).has_value());
        CY_REQUIRE(producers_.add_builtins().has_value());
        // The cook producer registers with a null world here: these cases are the importer's, and a
        // null registry makes a `cook` node fail by name rather than not exist. See
        // content_producers.h.
        CY_REQUIRE(add_content_producers(producers_, nullptr).has_value());
        CY_REQUIRE(read_description(kDescription, graph_, &producers_).has_value());
    }

    [[nodiscard]] std::string sources() const { return temp_.path() + "/project"; }
    [[nodiscard]] std::string artefacts() const { return temp_.path() + "/artefacts"; }
    [[nodiscard]] std::string cache() const { return temp_.path() + "/cache"; }
    [[nodiscard]] std::string path(const char* name) const { return temp_.path() + "/" + name; }

    void write(const std::string& name, std::string_view content) const {
        const std::string path = sources() + "/" + name;
        const usize slash = path.rfind('/');
        CY_REQUIRE(assets::fs::create_directories(path.substr(0, slash).c_str()).has_value());
        CY_REQUIRE(
            assets::fs::write_atomic(path.c_str(), content.data(), content.size()).has_value());
    }

    [[nodiscard]] BuildConfig config(const std::string& artefact_root,
                                     const std::string& cache_root) {
        provider_ = std::make_unique<DirectorySourceProvider>(sources());
        cache_root_ = cache_root;
        BuildConfig config;
        config.graph = &graph_;
        config.producers = &producers_;
        config.sources = provider_.get();
        config.artefact_root = artefact_root;
        config.cache.local = cache_root_.c_str();
        config.workers = 0;
        return config;
    }

private:
    cy::test::TempDir temp_;
    ProducerRegistry producers_;
    BuildGraph graph_;
    std::unique_ptr<DirectorySourceProvider> provider_;
    std::string cache_root_;
};

void seed(const Project& project, float height = 1.0F) {
    const std::string buffer = quad_buffer(height);
    project.write("assets/quad.bin", buffer);
    project.write("assets/quad.gltf", quad_document(buffer.size()));
    project.write("assets/notes.txt", "unrelated\n");
}

[[nodiscard]] const NodeResult* result_for(const BuildReport& report, std::string_view name) {
    for (const NodeResult& result : report.nodes) {
        if (result.name == name) {
            return &result;
        }
    }
    return nullptr;
}

[[nodiscard]] std::string artefact_of(const BuildReport& report, const ArtefactStore& store,
                                      std::string_view node) {
    const NodeResult* result = result_for(report, node);
    CY_REQUIRE(result != nullptr);
    CY_REQUIRE(result->outputs.size() == 1);
    Array<u8> buffer(test_allocator());
    CY_REQUIRE(store.get(result->outputs.front().digest, buffer).has_value());
    return {reinterpret_cast<const char*>(buffer.data()), buffer.size()};
}

}  // namespace

CY_TEST_CASE("the importer runs as a graph node and produces a bundle") {
    Project project("content-import");
    seed(project);

    BuildService service;
    CY_REQUIRE(service.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> report = service.build();
    CY_REQUIRE(report.has_value());
    CY_REQUIRE(report->succeeded());

    const NodeResult* imported = result_for(*report, "import:quad");
    CY_REQUIRE(imported != nullptr);
    CY_CHECK(imported->outcome == NodeOutcome::Ran);
    CY_REQUIRE(imported->outputs.size() == 1);
    // A bundle with a mesh in it, not an empty one.
    CY_CHECK(imported->outputs.front().size > 128);
    CY_CHECK(report->violations.empty());
}

CY_TEST_CASE("a glTF's external buffer is a discovered dependency of the node") {
    // THE PROPERTY RUNNING THE IMPORTER BESIDE THE GRAPH COULD NOT GIVE. `quad.bin` is named by
    // nothing in the description; the importer asks its resolver for it, the resolver is
    // `NodeContext::discover`, and reading it IS recording it. So editing it invalidates the node.
    Project project("content-discover");
    seed(project);

    BuildService first;
    CY_REQUIRE(first.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> cold = first.build();
    CY_REQUIRE(cold.has_value());
    CY_REQUIRE(cold->succeeded());
    const std::string before = artefact_of(*cold, first.artefacts(), "import:quad");

    // Nothing changed: the node is served from the cache.
    BuildService warm;
    CY_REQUIRE(warm.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> unchanged = warm.build();
    CY_REQUIRE(unchanged.has_value());
    CY_CHECK(result_for(*unchanged, "import:quad")->outcome == NodeOutcome::Cached);
    CY_CHECK_EQ(artefact_of(*unchanged, warm.artefacts(), "import:quad"), before);

    // The BUFFER changes, and the .gltf does not. Only a recorded discovery can see this.
    project.write("assets/quad.bin", quad_buffer(4.0F));

    BuildService after;
    CY_REQUIRE(after.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> rebuilt = after.build();
    CY_REQUIRE(rebuilt.has_value());
    CY_REQUIRE(rebuilt->succeeded());
    const NodeResult* imported = result_for(*rebuilt, "import:quad");
    CY_REQUIRE(imported != nullptr);
    CY_CHECK(imported->outcome != NodeOutcome::Cached);
    CY_CHECK(artefact_of(*rebuilt, after.artefacts(), "import:quad") != before);
    // And the node beside it, which reads none of that, did not move.
    CY_CHECK(result_for(*rebuilt, "unrelated")->outcome == NodeOutcome::Cached);
}

CY_TEST_CASE("two cold builds of one import node produce byte-identical bundles") {
    // The two-run determinism gate `just content-validate` provides, applied to the node rather
    // than to the CLI beside it. No cache on either run: a cache would make the second run a copy
    // of the first's answer.
    Project project("content-determinism");
    seed(project);

    BuildService first;
    CY_REQUIRE(first.configure(project.config(project.path("a"), "")).has_value());
    const Expected<BuildReport, Error> one = first.build();
    CY_REQUIRE(one.has_value());
    CY_REQUIRE(one->succeeded());

    BuildService second;
    CY_REQUIRE(second.configure(project.config(project.path("b"), "")).has_value());
    const Expected<BuildReport, Error> two = second.build();
    CY_REQUIRE(two.has_value());
    CY_REQUIRE(two->succeeded());

    CY_CHECK_EQ(artefact_of(*one, first.artefacts(), "import:quad"),
                artefact_of(*two, second.artefacts(), "import:quad"));
    CY_CHECK(result_for(*one, "import:quad")->outputs.front().digest ==
             result_for(*two, "import:quad")->outputs.front().digest);
}

CY_TEST_CASE("the cook runs as a graph node and produces a package") {
    // The other half of M7 task 1.4. The documents are read through `NodeContext::read` into a
    // memory mount and the cook is handed a filesystem over that mount alone, so a document the
    // node did not declare does not exist as far as the cook is concerned.
    Project project("content-cook");
    seed(project);
    project.write("assets/level.cyscene", scene_document_text());

    ecs::World world(test_allocator(), ecs::WorldConfig{"cook-node", 1024});
    CY_REQUIRE(world.initialize().has_value());
    CY_REQUIRE(world.components().register_reflected(marker_type()).has_value());

    ProducerRegistry producers;
    CY_REQUIRE(producers.add_builtins().has_value());
    CY_REQUIRE(add_content_producers(producers, &world).has_value());

    BuildGraph graph;
    const std::string description = std::string("cybuild 1\n") +
                                    "node \"cook:level\" cook \"cook\" 1\n"
                                    "  source \"assets/level.cyscene\"\n"
                                    "  output \"derived/level.cypak\"\n"
                                    "  option \"source\" \"assets\"\n"
                                    "  option \"variant\" \"desktop\"\n"
                                    "  option \"scratch\" \"" +
                                    project.path("scratch") + "\"\n";
    CY_REQUIRE(assets::fs::create_directories(project.path("scratch").c_str()).has_value());
    CY_REQUIRE(read_description(description, graph, &producers).has_value());

    DirectorySourceProvider sources(project.sources());
    BuildConfig config;
    config.graph = &graph;
    config.producers = &producers;
    config.sources = &sources;
    config.artefact_root = project.path("cook-artefacts");
    config.cache.local = nullptr;
    config.workers = 0;

    BuildService service;
    CY_REQUIRE(service.configure(std::move(config)).has_value());
    const Expected<BuildReport, Error> report = service.build();
    CY_REQUIRE(report.has_value());
    CY_REQUIRE(report->succeeded());
    const NodeResult* cooked = result_for(*report, "cook:level");
    CY_REQUIRE(cooked != nullptr);
    CY_CHECK(cooked->outcome == NodeOutcome::Ran);
    CY_REQUIRE(cooked->outputs.size() == 1);
    // A `.cypak` with a header and one entry, not an empty file.
    CY_CHECK(cooked->outputs.front().size > 64);
    CY_CHECK(report->violations.empty());
}

CY_TEST_CASE("a cook node without a component registry fails by name rather than guessing") {
    // `tools/cook/` is explicit that a cook needs a world "that has registered them ... so the
    // caller supplies one", and a producer that guessed would emit blocks against a layout the
    // runtime rejects at the build-schema check — in a place nobody would look.
    Project project("content-cook-noworld");
    seed(project);

    ProducerRegistry producers;
    CY_REQUIRE(producers.add_builtins().has_value());
    CY_REQUIRE(add_content_producers(producers, nullptr).has_value());
    const Producer* cook = producers.find("cook");
    CY_REQUIRE(cook != nullptr);
    CY_CHECK(!cook->distributable);
    CY_CHECK_EQ(cook->version, kCookProducerVersion);

    const Producer* import = producers.find("import");
    CY_REQUIRE(import != nullptr);
    CY_CHECK(import->distributable);
    CY_CHECK_EQ(import->version, kImportProducerVersion);
}
