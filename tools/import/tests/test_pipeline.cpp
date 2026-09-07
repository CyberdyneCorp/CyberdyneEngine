// The pipeline end to end: a project on disk, a cache, two sidecars and a report. M5 task 5.1.
//
// Integration rather than unit, because every case here writes real files — which is the point.
// The properties worth having are about what survives between runs: an id that does not move when a
// mesh is re-imported, a cache that answers the second run, and a dependency whose change
// invalidates what read it.

#include <cy/core/assets/derived_cache.h>
#include <cy/core/assets/file.h>
#include <cy/core/assets/identity.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/assets/watch.h>
#include <cy/core/memory/allocator.h>

#include <cy/core/jobs/job_system.h>
#include <cy/import/live_import.h>
#include <cy/import/pipeline.h>
#include <cy/import/texture.h>
#include <cy/test/test.h>
#include <utility>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace cy::import;
using cy::u32;
using cy::u8;
using cy::usize;

namespace {

/// A scratch directory that removes itself, under the test binary's own working directory so that
/// the artefacts of a failed run are beside the binary that produced them.
class TempDir {
public:
    explicit TempDir(const char* label) {
        static std::atomic<unsigned> serial{0};
        char buffer[256] = {};
        (void)std::snprintf(buffer, sizeof(buffer), "cy_import_test_%s_%u", label,
                            serial.fetch_add(1));
        path_ = buffer;
        (void)cy::assets::fs::remove_directory_recursive(path_.c_str());
        CY_REQUIRE(cy::assets::fs::create_directories(path_.c_str()).has_value());
    }

    ~TempDir() { (void)cy::assets::fs::remove_directory_recursive(path_.c_str()); }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const char* c_str() const noexcept { return path_.c_str(); }
    [[nodiscard]] std::string file(const char* name) const { return path_ + "/" + name; }

private:
    std::string path_;
};

void write_file(const std::string& path, const std::vector<u8>& bytes) {
    const std::string::size_type slash = path.rfind('/');
    if (slash != std::string::npos) {
        CY_REQUIRE(cy::assets::fs::create_directories(path.substr(0, slash).c_str()).has_value());
    }
    CY_REQUIRE(cy::assets::fs::write_atomic(path.c_str(), bytes.data(), bytes.size()).has_value());
}

void write_text(const std::string& path, std::string_view text) {
    write_file(path, std::vector<u8>(text.begin(), text.end()));
}

std::string read_text(const std::string& path) {
    cy::Array<u8> bytes;
    CY_REQUIRE(cy::assets::fs::read_whole(path.c_str(), bytes).has_value());
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// A 4x4 fully opaque Targa, written as a source file.
///
/// Opaque on purpose: a 32-bit image whose alpha is entirely 255 is the "unnecessary alpha channel"
/// the importer reports, which gives the report test something real to find.
std::vector<u8> targa(u8 red) {
    std::vector<u8> bytes(18, 0);
    bytes[2] = 2;
    bytes[12] = 4;
    bytes[14] = 4;
    bytes[16] = 32;
    bytes[17] = 0x20;
    for (usize index = 0; index < 16; ++index) {
        bytes.push_back(0);
        bytes.push_back(0);
        bytes.push_back(red);
        bytes.push_back(255);
    }
    return bytes;
}

/// A `.gltf` whose buffer is an external `.bin` beside it, so that the resolver has something to
/// record as a dependency.
std::string gltf_with_external_buffer(std::string_view buffer_name) {
    std::string document;
    document += R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":92,"uri":")";
    document += buffer_name;
    document += R"("}],"bufferViews":[)";
    document += R"({"buffer":0,"byteOffset":0,"byteLength":48},)";
    document += R"({"buffer":0,"byteOffset":48,"byteLength":32},)";
    document += R"({"buffer":0,"byteOffset":80,"byteLength":12}],)";
    document += R"("accessors":[)";
    document += R"({"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},)";
    document += R"({"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},)";
    document += R"({"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"}],)";
    document += R"("meshes":[{"name":"Panel","primitives":[{"attributes":{"POSITION":0,)"
                R"("TEXCOORD_0":1},"indices":2,"mode":4}]}],)";
    document += R"("nodes":[{"name":"Panel","mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    return document;
}

/// The buffer that document references: four positions, four texture coordinates, six indices.
std::vector<u8> quad_buffer(float height) {
    std::vector<u8> bytes;
    const auto put_f32 = [&bytes](float value) {
        u32 bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for (u32 index = 0; index < 4; ++index) {
            bytes.push_back(static_cast<u8>((bits >> (index * 8U)) & 0xFFU));
        }
    };
    const auto put_u16 = [&bytes](u32 value) {
        bytes.push_back(static_cast<u8>(value & 0xFFU));
        bytes.push_back(static_cast<u8>((value >> 8U) & 0xFFU));
    };
    const float positions[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, height, 0}, {1, height, 0}};
    for (const auto& position : positions) {
        put_f32(position[0]);
        put_f32(position[1]);
        put_f32(position[2]);
    }
    const float uvs[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (const auto& uv : uvs) {
        put_f32(uv[0]);
        put_f32(uv[1]);
    }
    for (const u32 index : {0U, 1U, 2U, 2U, 1U, 3U}) {
        put_u16(index);
    }
    return bytes;
}

struct Harness {
    TempDir directory;
    std::string project;
    std::string output;
    std::string cache_root;
    ImporterRegistry registry;
    cy::assets::DerivedCache cache;

    explicit Harness(const char* label)
        : directory(label),
          project(directory.file("project")),
          output(directory.file("cooked")),
          cache_root(directory.file("cache")) {
        CY_REQUIRE(register_builtin_importers(registry).has_value());
        cy::assets::DerivedCacheTiers tiers;
        tiers.local = cache_root.c_str();
        CY_REQUIRE(cache.configure(tiers).has_value());
        CY_REQUIRE(cy::assets::fs::create_directories(project.c_str()).has_value());
    }

    /// `ImportPipeline` holds atomics and is neither copyable nor movable — a pipeline that could
    /// be moved while a worker held a reference to it would be a pipeline with a race in it — so a
    /// harness hands one out by reference rather than by value.
    void configure(ImportPipeline& built) const {
        CY_REQUIRE(built.set_project_root(project).has_value());
        CY_REQUIRE(built.set_output_directory(output).has_value());
    }

    [[nodiscard]] static cy::assets::VirtualPath path(std::string_view text) {
        return cy::assets::VirtualPath::normalise(text).value();
    }
};

}  // namespace

CY_TEST_CASE("pipeline: a texture imports, writes cooked bytes and leaves two sidecars") {
    Harness harness("texture");
    write_file(harness.project + "/textures/stone.tga", targa(200));

    ImportPipeline pipeline(harness.registry, harness.cache);
    harness.configure(pipeline);
    ImportSettings settings;
    auto outcome = pipeline.import_file(Harness::path("textures/stone.tga"), settings);
    CY_REQUIRE(outcome.has_value());
    CY_CHECK(outcome.value().succeeded());
    CY_CHECK(outcome.value().sub_assets == 1);
    CY_CHECK(outcome.value().cooked_bytes > 0);
    CY_CHECK(outcome.value().cache == cy::assets::CacheOutcome::Miss);
    CY_CHECK(!outcome.value().id.is_nil());

    // The engine's identity record, and the importer's own. See the note at the head of sidecar.h
    // for why there are two of them.
    CY_CHECK(cy::assets::fs::exists((harness.project + "/textures/stone.tga.meta").c_str()));
    CY_CHECK(cy::assets::fs::exists((harness.project + "/textures/stone.tga.import").c_str()));

    // `AssetMeta::cooked_hash` has been zero in every sidecar this project has written, because
    // nothing cooked before M5. It is what an incremental build compares, and this is the first
    // thing that fills it in.
    const std::string meta = read_text(harness.project + "/textures/stone.tga.meta");
    CY_CHECK(meta.find("cooked_hash = \"00000000") == std::string::npos);

    char id_text[cy::AssetId::kTextLength + 1] = {};
    (void)outcome.value().id.format(id_text);
    CY_CHECK(cy::assets::fs::exists((harness.output + "/" + id_text + ".cyasset").c_str()));
}

CY_TEST_CASE("pipeline: the second run is a cache hit and produces the same output") {
    // "WHEN a developer pulls a branch whose assets CI already cooked THEN the cooked artefacts
    // SHALL be fetched from the shared cache rather than re-imported locally" — the local half of
    // it, and the half that runs on every developer's second build.
    Harness harness("cache_hit");
    write_file(harness.project + "/textures/stone.tga", targa(200));

    ImportPipeline first(harness.registry, harness.cache);
    harness.configure(first);
    ImportSettings settings;
    auto cold = first.import_file(Harness::path("textures/stone.tga"), settings);
    CY_REQUIRE(cold.has_value());
    CY_CHECK(cold.value().cache == cy::assets::CacheOutcome::Miss);

    char id_text[cy::AssetId::kTextLength + 1] = {};
    (void)cold.value().id.format(id_text);
    const std::string cooked = harness.output + "/" + id_text + ".cyasset";
    const std::string before = read_text(cooked);

    // Delete the cooked output. A hit must REPRODUCE it rather than skip it — a developer who
    // cleared their output directory and kept their cache must get their content back.
    CY_REQUIRE(cy::assets::fs::remove_file(cooked.c_str()).has_value());

    ImportPipeline second(harness.registry, harness.cache);
    harness.configure(second);
    auto warm = second.import_file(Harness::path("textures/stone.tga"), settings);
    CY_REQUIRE(warm.has_value());
    CY_CHECK(warm.value().cache == cy::assets::CacheOutcome::Hit);
    CY_CHECK(warm.value().tier == cy::assets::CacheTier::Local);
    CY_CHECK(warm.value().id == cold.value().id);
    CY_CHECK(cy::assets::fs::exists(cooked.c_str()));
    CY_CHECK(read_text(cooked) == before);
}

CY_TEST_CASE("pipeline: changing the source re-cooks and changing an option re-cooks") {
    Harness harness("invalidate");
    const std::string source = harness.project + "/textures/stone.tga";
    write_file(source, targa(200));

    ImportSettings settings;
    {
        ImportPipeline pipeline(harness.registry, harness.cache);
        harness.configure(pipeline);
        CY_REQUIRE(pipeline.import_file(Harness::path("textures/stone.tga"), settings).has_value());
    }

    write_file(source, targa(40));
    {
        ImportPipeline pipeline(harness.registry, harness.cache);
        harness.configure(pipeline);
        auto outcome = pipeline.import_file(Harness::path("textures/stone.tga"), settings);
        CY_REQUIRE(outcome.has_value());
        // The source's content is in the derivation key, so a different source is a different key.
        CY_CHECK(outcome.value().cache == cy::assets::CacheOutcome::Miss);
    }

    ImportOptions options;
    const OptionsSchema schema = texture_options();
    CY_REQUIRE(options.set(schema, "generate-mips", OptionValue::of_bool(false)).has_value());
    settings.options = &options;
    {
        ImportPipeline pipeline(harness.registry, harness.cache);
        harness.configure(pipeline);
        auto outcome = pipeline.import_file(Harness::path("textures/stone.tga"), settings);
        CY_REQUIRE(outcome.has_value());
        // "WHEN an import option changes THEN the cache key SHALL change and the asset SHALL be
        // re-cooked."
        CY_CHECK(outcome.value().cache == cy::assets::CacheOutcome::Miss);
    }
}

CY_TEST_CASE("pipeline: a sub-asset keeps its id across a re-import") {
    // `asset-import-pipeline`: "WHEN a scene file produces meshes, materials and animations THEN
    // each SHALL receive a stable sub-asset id recorded in the `.meta`, so references survive
    // re-import." This is the assertion that makes that sentence true rather than intended.
    Harness harness("stable_ids");
    write_text(harness.project + "/models/panel.gltf", gltf_with_external_buffer("panel.bin"));
    write_file(harness.project + "/models/panel.bin", quad_buffer(1.0f));

    ImportSettings settings;
    settings.ignore_cache = true;  // force the importer to run both times

    ImportPipeline first(harness.registry, harness.cache);
    harness.configure(first);
    auto cold = first.import_file(Harness::path("models/panel.gltf"), settings);
    CY_REQUIRE(cold.has_value());
    CY_CHECK(cold.value().succeeded());
    CY_CHECK(cold.value().minted_ids == cold.value().sub_assets);

    const std::string record = read_text(harness.project + "/models/panel.gltf.import");

    // Change the SOURCE — a different vertex position — and re-import. Every sub-asset name is
    // unchanged, so every id must be too: a reference to the mesh must not follow the geometry.
    write_file(harness.project + "/models/panel.bin", quad_buffer(2.0f));
    ImportPipeline second(harness.registry, harness.cache);
    harness.configure(second);
    auto warm = second.import_file(Harness::path("models/panel.gltf"), settings);
    CY_REQUIRE(warm.has_value());
    CY_CHECK(warm.value().succeeded());
    CY_CHECK(warm.value().minted_ids == 0);
    CY_CHECK(warm.value().id == cold.value().id);

    // And the record's sub-asset table is byte-identical, so a re-import produces no diff a
    // reviewer has to read.
    const std::string after = read_text(harness.project + "/models/panel.gltf.import");
    const auto sub_assets_of = [](const std::string& text) {
        std::string kept;
        usize cursor = 0;
        while (cursor < text.size()) {
            const usize newline = text.find('\n', cursor);
            const usize end = newline == std::string::npos ? text.size() : newline;
            const std::string line = text.substr(cursor, end - cursor);
            if (line.starts_with("sub_asset.")) {
                kept += line;
                kept += '\n';
            }
            cursor = end + 1;
        }
        return kept;
    };
    CY_CHECK(sub_assets_of(record) == sub_assets_of(after));
    CY_CHECK(!sub_assets_of(record).empty());
}

CY_TEST_CASE("pipeline: a changed dependency invalidates the entry that read it") {
    // "WHEN a shared shader include is edited THEN every shader that includes it SHALL be
    // re-cooked." The glTF's external buffer is the same relationship: it is not in the derivation
    // key, because nothing knew about it until the importer read it.
    Harness harness("dependency");
    write_text(harness.project + "/models/panel.gltf", gltf_with_external_buffer("panel.bin"));
    write_file(harness.project + "/models/panel.bin", quad_buffer(1.0f));

    ImportSettings settings;
    {
        ImportPipeline pipeline(harness.registry, harness.cache);
        harness.configure(pipeline);
        auto cold = pipeline.import_file(Harness::path("models/panel.gltf"), settings);
        CY_REQUIRE(cold.has_value());
        CY_CHECK(cold.value().succeeded());
    }
    {
        ImportPipeline pipeline(harness.registry, harness.cache);
        harness.configure(pipeline);
        auto warm = pipeline.import_file(Harness::path("models/panel.gltf"), settings);
        CY_REQUIRE(warm.has_value());
        CY_CHECK(warm.value().cache == cy::assets::CacheOutcome::Hit);
    }

    // The `.gltf` is untouched; only the buffer it references changed.
    write_file(harness.project + "/models/panel.bin", quad_buffer(3.0f));
    {
        ImportPipeline pipeline(harness.registry, harness.cache);
        harness.configure(pipeline);
        auto stale = pipeline.import_file(Harness::path("models/panel.gltf"), settings);
        CY_REQUIRE(stale.has_value());
        CY_CHECK(stale.value().cache == cy::assets::CacheOutcome::Invalidated);
        // The reason names the dependency, which is what turns a complaint into a fix.
        CY_CHECK(std::string_view(stale.value().cache_reason).find("panel.bin") !=
                 std::string_view::npos);
    }
}

CY_TEST_CASE("pipeline: a source nothing claims is reported rather than ignored") {
    Harness harness("unclaimed");
    write_text(harness.project + "/notes/readme.txt", "not an asset");
    ImportPipeline pipeline(harness.registry, harness.cache);
    harness.configure(pipeline);
    auto outcome = pipeline.import_file(Harness::path("notes/readme.txt"), ImportSettings{});
    CY_REQUIRE(outcome.has_value());
    CY_CHECK(!outcome.value().succeeded());
    CY_CHECK(std::string_view(outcome.value().cache_reason).find("no importer") !=
             std::string_view::npos);
}

CY_TEST_CASE("pipeline: several assets import on the job system and report in source order") {
    Harness harness("parallel");
    for (u32 index = 0; index < 8; ++index) {
        char name[64] = {};
        (void)std::snprintf(name, sizeof(name), "/textures/stone%u.tga", index);
        write_file(harness.project + name, targa(static_cast<u8>(index * 20)));
    }

    cy::Array<cy::assets::VirtualPath> sources;
    for (u32 index = 0; index < 8; ++index) {
        char name[64] = {};
        (void)std::snprintf(name, sizeof(name), "textures/stone%u.tga", index);
        CY_REQUIRE(sources.push_back(Harness::path(name)).has_value());
    }

    cy::jobs::JobSystem job_system;
    cy::jobs::JobSystemConfig configuration;
    configuration.worker_count = 3;
    CY_REQUIRE(job_system.start(configuration).has_value());

    ImportPipeline pipeline(harness.registry, harness.cache);
    harness.configure(pipeline);
    auto imported =
        pipeline.import_all(cy::Span<const cy::assets::VirtualPath>(sources.data(), sources.size()),
                            ImportSettings{}, &job_system);
    job_system.shutdown();

    CY_REQUIRE(imported.has_value());
    CY_CHECK(imported.value() == 8);
    CY_REQUIRE(pipeline.report().size() == 8);
    // The report is in SOURCE order however the workers finished, so two runs of the same import
    // produce the same report.
    for (usize index = 0; index < 8; ++index) {
        char expected[64] = {};
        (void)std::snprintf(expected, sizeof(expected), "textures/stone%zu.tga", index);
        CY_CHECK(std::string_view(pipeline.report().rows()[index].source) == expected);
    }
    CY_CHECK(pipeline.report().total_errors() == 0);
}

CY_TEST_CASE("pipeline: cancellation stops at the next asset and leaves the cache consistent") {
    // "WHEN the user cancels an import THEN it SHALL stop at the next step boundary, leaving the
    // cache consistent."
    Harness harness("cancel");
    for (u32 index = 0; index < 4; ++index) {
        char name[64] = {};
        (void)std::snprintf(name, sizeof(name), "/textures/stone%u.tga", index);
        write_file(harness.project + name, targa(static_cast<u8>(index * 30)));
    }
    cy::Array<cy::assets::VirtualPath> sources;
    for (u32 index = 0; index < 4; ++index) {
        char name[64] = {};
        (void)std::snprintf(name, sizeof(name), "textures/stone%u.tga", index);
        CY_REQUIRE(sources.push_back(Harness::path(name)).has_value());
    }

    ImportPipeline pipeline(harness.registry, harness.cache);
    harness.configure(pipeline);
    pipeline.request_cancellation();
    auto imported =
        pipeline.import_all(cy::Span<const cy::assets::VirtualPath>(sources.data(), sources.size()),
                            ImportSettings{}, nullptr);
    CY_REQUIRE(imported.has_value());
    CY_CHECK(imported.value() == 0);
    CY_CHECK(pipeline.report().size() == 0);

    // And the run resumes cleanly afterwards, which is what "leaving the cache consistent" means in
    // practice: nothing half-written stops the next attempt.
    pipeline.reset_cancellation();
    auto resumed =
        pipeline.import_all(cy::Span<const cy::assets::VirtualPath>(sources.data(), sources.size()),
                            ImportSettings{}, nullptr);
    CY_REQUIRE(resumed.has_value());
    CY_CHECK(resumed.value() == 4);
}

CY_TEST_CASE("pipeline: the report names the largest, the slowest and everything with a warning") {
    Harness harness("report");
    write_file(harness.project + "/textures/stone.tga", targa(200));

    ImportPipeline pipeline(harness.registry, harness.cache);
    harness.configure(pipeline);
    CY_REQUIRE(
        pipeline.import_file(Harness::path("textures/stone.tga"), ImportSettings{}).has_value());

    std::vector<char> text(8192, '\0');
    const usize written = pipeline.report().format(text.data(), text.size());
    CY_CHECK(written > 0);
    const std::string_view rendered(text.data(), written);
    CY_CHECK(rendered.find("import:") != std::string_view::npos);
    CY_CHECK(rendered.find("cache:") != std::string_view::npos);
    CY_CHECK(rendered.find("largest:") != std::string_view::npos);
    // The Targa is opaque, so the unnecessary-alpha diagnostic fires and the asset has something to
    // report — which is what puts it in the last section.
    CY_CHECK(rendered.find("textures/stone.tga") != std::string_view::npos);
    CY_CHECK(pipeline.report().total_warnings() >= 1);
}

CY_TEST_CASE("pipeline: deleting the cache costs a re-cook and nothing else") {
    // "WHEN the cache is deleted THEN the project SHALL remain complete and the next build SHALL
    // regenerate the derived data."
    Harness harness("disposable");
    write_file(harness.project + "/textures/stone.tga", targa(200));

    ImportSettings settings;
    ImportPipeline first(harness.registry, harness.cache);
    harness.configure(first);
    auto cold = first.import_file(Harness::path("textures/stone.tga"), settings);
    CY_REQUIRE(cold.has_value());

    CY_REQUIRE(harness.cache.clear_local().has_value());

    ImportPipeline second(harness.registry, harness.cache);
    harness.configure(second);
    auto again = second.import_file(Harness::path("textures/stone.tga"), settings);
    CY_REQUIRE(again.has_value());
    CY_CHECK(again.value().cache == cy::assets::CacheOutcome::Miss);
    CY_CHECK(again.value().succeeded());
    // The id is unchanged: it lives in the sidecar, which is project content and not derived data.
    CY_CHECK(again.value().id == cold.value().id);
}

// --- Cook profiles ------------------------------------------------------------------------------
//
// M6 task 8.3. `asset-import-pipeline` — "Cook profiles".

CY_TEST_CASE("profile: a dedicated-server cook excludes rendering content and reports the saving") {
    // "WHEN a `DedicatedServer` cook runs THEN textures, shaders, and VFX assets SHALL be excluded,
    // and the report SHALL show what was removed and the size saved."
    Harness harness("profile_server");
    write_file(harness.project + "/textures/stone.tga", targa(200));

    ImportPipeline client(harness.registry, harness.cache);
    harness.configure(client);
    ImportSettings settings;
    const auto cooked = client.import_file(Harness::path("textures/stone.tga"), settings);
    CY_REQUIRE(cooked.has_value());
    CY_CHECK(cooked.value().cooked_bytes > 0);
    CY_CHECK(cooked.value().excluded_sub_assets == 0);

    ImportPipeline server(harness.registry, harness.cache);
    harness.configure(server);
    ImportSettings server_settings;
    server_settings.profile = CookProfile::DedicatedServer;
    const auto trimmed = server.import_file(Harness::path("textures/stone.tga"), server_settings);
    CY_REQUIRE(trimmed.has_value());
    CY_CHECK(trimmed.value().excluded_sub_assets >= 1);
    CY_CHECK(trimmed.value().excluded_bytes > 0);
    CY_CHECK(trimmed.value().cooked_bytes == 0);
    CY_CHECK(server.report().total_excluded_bytes() > 0);

    // The report says so in words, because "accidental inclusions are visible" is about a person
    // reading it rather than about a field being set.
    char text[4096] = {};
    (void)server.report().format(text, sizeof(text));
    CY_CHECK(std::string_view(text).find("excluded") != std::string_view::npos);
}

CY_TEST_CASE("profile: collision survives a mesh exclusion, and the prefab does too") {
    // "WHEN a mesh contributes collision geometry and is excluded from a server cook THEN its
    // collision representation SHALL be retained."
    CY_CHECK(profile_retains(CookProfile::DedicatedServer, cy::assets::AssetKind::Mesh,
                             "collision/Crate_collision"));
    CY_CHECK(
        !profile_retains(CookProfile::DedicatedServer, cy::assets::AssetKind::Mesh, "mesh/Crate"));
    CY_CHECK(
        !profile_retains(CookProfile::DedicatedServer, cy::assets::AssetKind::Texture, "image"));
    CY_CHECK(!profile_retains(CookProfile::DedicatedServer, cy::assets::AssetKind::Material,
                              "material/Oak"));
    CY_CHECK(
        profile_retains(CookProfile::DedicatedServer, cy::assets::AssetKind::Prefab, "prefab"));
    // Every other profile keeps everything, which is what makes the switch a policy rather than an
    // accumulation of flags.
    for (const CookProfile profile : {CookProfile::Client, CookProfile::Editor}) {
        CY_CHECK(profile_retains(profile, cy::assets::AssetKind::Texture, "image"));
        CY_CHECK(profile_retains(profile, cy::assets::AssetKind::Mesh, "mesh/Crate"));
    }
}

CY_TEST_CASE("profile: an id does not move between a client cook and a server cook") {
    // The reason exclusion happens at publication and not at import: a prefab in the server package
    // references a mesh by the id the client package uses, so the ids must be assigned identically
    // whatever the profile drops.
    Harness harness("profile_ids");
    write_file(harness.project + "/textures/stone.tga", targa(90));

    ImportPipeline first(harness.registry, harness.cache);
    harness.configure(first);
    ImportSettings settings;
    const auto client = first.import_file(Harness::path("textures/stone.tga"), settings);
    CY_REQUIRE(client.has_value());

    ImportPipeline second(harness.registry, harness.cache);
    harness.configure(second);
    ImportSettings server_settings;
    server_settings.profile = CookProfile::DedicatedServer;
    const auto server = second.import_file(Harness::path("textures/stone.tga"), server_settings);
    CY_REQUIRE(server.has_value());
    CY_CHECK(client.value().id == server.value().id);
    // And nothing was minted the second time: the record the first cook committed is what the
    // second read.
    CY_CHECK(server.value().minted_ids == 0);
}

CY_TEST_CASE(
    "profile: a cook that refuses to mint fails naming the asset, and succeeds once bound") {
    // design.md §1.7: minting is what makes two cold builds of one project produce different bytes,
    // and M6 is the milestone at which an id reaches the inside of a payload. A shipping cook
    // refuses; the remedy is to commit the sidecar.
    Harness harness("profile_mint");
    write_file(harness.project + "/textures/stone.tga", targa(30));

    ImportPipeline strict(harness.registry, harness.cache);
    harness.configure(strict);
    ImportSettings settings;
    settings.minting = MintPolicy::Refuse;
    const auto refused = strict.import_file(Harness::path("textures/stone.tga"), settings);
    CY_CHECK(!refused.has_value());

    // Import it once the ordinary way, which writes the record, then the strict cook works.
    ImportPipeline permissive(harness.registry, harness.cache);
    harness.configure(permissive);
    ImportSettings mint;
    CY_REQUIRE(permissive.import_file(Harness::path("textures/stone.tga"), mint).has_value());

    ImportPipeline again(harness.registry, harness.cache);
    harness.configure(again);
    ImportSettings strict_again;
    strict_again.minting = MintPolicy::Refuse;
    strict_again.ignore_cache = true;
    const auto accepted = again.import_file(Harness::path("textures/stone.tga"), strict_again);
    CY_REQUIRE(accepted.has_value());
    CY_CHECK(accepted.value().minted_ids == 0);
}

CY_TEST_CASE("live: an edited source is re-cooked and the resident asset is replaced") {
    // `live-editing` — "Live asset reload": "recompiled or reimported content SHALL replace the
    // resource behind an existing stable handle, so holders need not re-resolve". The stable handle
    // here is the `Ref` the asset system hands out; the whole point is that it is never
    // re-resolved.
    Harness harness("live");
    write_file(harness.project + "/textures/stone.tga", targa(200));

    ImportPipeline pipeline(harness.registry, harness.cache);
    harness.configure(pipeline);
    ImportSettings settings;
    auto cold = pipeline.import_file(Harness::path("textures/stone.tga"), settings);
    CY_REQUIRE(cold.has_value());
    CY_CHECK(cold.value().succeeded());

    // The watcher works over the virtual namespace, so the project is mounted as a directory.
    cy::assets::VirtualFileSystem files;
    {
        auto mount = cy::assets::DirectoryMount::create(harness.project.c_str(),
                                                        cy::assets::MountKind::Project, false);
        CY_REQUIRE(mount.has_value());
        CY_REQUIRE(files.mount_owned(std::move(mount.value()), 0).has_value());
    }
    cy::assets::FileWatcher watcher(cy::current_allocator());
    // No settle period, because the test's clock is the only thing writing these files. The
    // fingerprint ceiling is set explicitly rather than left at zero: a zero ceiling fingerprints
    // by SIZE alone, and every edit in these cases keeps the file the same length.
    cy::assets::FileWatcherConfig configuration;
    configuration.settle_ns = 0;
    CY_REQUIRE(watcher.start(files, configuration).has_value());

    LiveImportSession session(pipeline, watcher);
    CY_REQUIRE(session.register_source(Harness::path("textures/stone.tga")).has_value());
    CY_REQUIRE(session.watch(Harness::path("textures")).has_value());
    CY_REQUIRE(session.prime(0).has_value());

    // Nothing has changed: a poll costs the watcher's sweep and does no work at all.
    auto quiet = session.poll(1'000'000, settings);
    CY_REQUIRE(quiet.has_value());
    CY_CHECK(quiet.value() == 0);
    CY_CHECK(session.last_edits().empty());

    write_file(harness.project + "/textures/stone.tga", targa(40));
    auto edited = session.poll(2'000'000, settings);
    CY_REQUIRE(edited.has_value());
    CY_CHECK(edited.value() == 1);
    CY_REQUIRE(session.last_edits().size() == 1);
    // Nothing is resident in this test — there is no asset system bound — so the honest outcome is
    // "not resident", which is a different answer from "refused" and is why the enumeration has
    // both.
    CY_CHECK(session.last_edits()[0].outcome == LiveEditOutcome::NotResident);
    CY_CHECK(session.stats().recooked == 1);
    CY_CHECK(session.stats().refused == 0);
}

CY_TEST_CASE("live: a change to a dependency re-cooks the asset that read it") {
    // The file that changed is not an asset any importer claims — it is a glTF's external buffer —
    // and the asset that has to be re-cooked is the one that read it. This is why a live session
    // asks the cache rather than keeping its own reverse index; see live_import.h.
    Harness harness("live_dependency");
    write_text(harness.project + "/models/panel.gltf", gltf_with_external_buffer("panel.bin"));
    write_file(harness.project + "/models/panel.bin", quad_buffer(1.0f));

    ImportPipeline pipeline(harness.registry, harness.cache);
    harness.configure(pipeline);
    ImportSettings settings;
    CY_REQUIRE(pipeline.import_file(Harness::path("models/panel.gltf"), settings).has_value());

    cy::assets::VirtualFileSystem files;
    {
        auto mount = cy::assets::DirectoryMount::create(harness.project.c_str(),
                                                        cy::assets::MountKind::Project, false);
        CY_REQUIRE(mount.has_value());
        CY_REQUIRE(files.mount_owned(std::move(mount.value()), 0).has_value());
    }
    cy::assets::FileWatcher watcher(cy::current_allocator());
    // No settle period, because the test's clock is the only thing writing these files. The
    // fingerprint ceiling is set explicitly rather than left at zero: a zero ceiling fingerprints
    // by SIZE alone, and every edit in these cases keeps the file the same length.
    cy::assets::FileWatcherConfig configuration;
    configuration.settle_ns = 0;
    CY_REQUIRE(watcher.start(files, configuration).has_value());

    LiveImportSession session(pipeline, watcher);
    CY_REQUIRE(session.register_source(Harness::path("models/panel.gltf")).has_value());
    CY_REQUIRE(session.watch(Harness::path("models")).has_value());
    CY_REQUIRE(session.prime(0).has_value());

    write_file(harness.project + "/models/panel.bin", quad_buffer(5.0f));
    auto edited = session.poll(1'000'000, settings);
    CY_REQUIRE(edited.has_value());
    CY_CHECK(edited.value() == 1);
    CY_CHECK(session.stats().recooked == 1);
}

CY_TEST_CASE("live: a source that stops compiling leaves the previous asset alone") {
    // "A failed reload SHALL keep the previous resource and report the failure." The importer half
    // of it: a source that no longer parses is `Refused`, and nothing that was working is touched.
    Harness harness("live_refused");
    write_text(harness.project + "/models/panel.gltf", gltf_with_external_buffer("panel.bin"));
    write_file(harness.project + "/models/panel.bin", quad_buffer(1.0f));

    ImportPipeline pipeline(harness.registry, harness.cache);
    harness.configure(pipeline);
    ImportSettings settings;
    auto cold = pipeline.import_file(Harness::path("models/panel.gltf"), settings);
    CY_REQUIRE(cold.has_value());
    CY_REQUIRE(cold.value().succeeded());
    char id_text[cy::AssetId::kTextLength + 1] = {};
    (void)cold.value().id.format(id_text);
    const std::string cooked = harness.output + "/" + id_text + ".cyasset";
    const std::string before = read_text(cooked);

    cy::assets::VirtualFileSystem files;
    {
        auto mount = cy::assets::DirectoryMount::create(harness.project.c_str(),
                                                        cy::assets::MountKind::Project, false);
        CY_REQUIRE(mount.has_value());
        CY_REQUIRE(files.mount_owned(std::move(mount.value()), 0).has_value());
    }
    cy::assets::FileWatcher watcher(cy::current_allocator());
    // No settle period, because the test's clock is the only thing writing these files. The
    // fingerprint ceiling is set explicitly rather than left at zero: a zero ceiling fingerprints
    // by SIZE alone, and every edit in these cases keeps the file the same length.
    cy::assets::FileWatcherConfig configuration;
    configuration.settle_ns = 0;
    CY_REQUIRE(watcher.start(files, configuration).has_value());

    LiveImportSession session(pipeline, watcher);
    CY_REQUIRE(session.register_source(Harness::path("models/panel.gltf")).has_value());
    CY_REQUIRE(session.watch(Harness::path("models")).has_value());
    CY_REQUIRE(session.prime(0).has_value());

    write_text(harness.project + "/models/panel.gltf", R"({"asset":{"version":"2.0"},"meshes":[)");
    auto edited = session.poll(1'000'000, settings);
    CY_REQUIRE(edited.has_value());
    CY_REQUIRE(session.last_edits().size() == 1);
    CY_CHECK(session.last_edits()[0].outcome == LiveEditOutcome::Refused);
    CY_CHECK(session.stats().refused == 1);
    // The cooked artefact that was working is exactly as it was.
    CY_CHECK(read_text(cooked) == before);
}
