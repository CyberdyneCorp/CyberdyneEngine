// Packaging, chunk-level patching, and an application that is interrupted. M6 tasks 7.5 and 7.6.
//
// The milestone's exit criterion is stated as a test rather than an argument: **a patch applies
// atomically and rolls back cleanly when interrupted.** So this suite interrupts at every stage,
// twice over:
//
//   * SOFTLY, by returning at the stage, which exercises the rollback path;
//   * HARD, by forking and calling `_exit` at the stage, which runs no destructor, flushes no
//     stream and removes no temporary. Only the second one proves the property the specification
//     states, because a test that let the process tidy up would be testing the tidying.
//
// After each, the installation is opened afresh and must still name the previous build, verify
// every chunk of it, and hand back the previous build's bytes.

#include <cy/build/description.h>
#include <cy/build/package.h>
#include <cy/build/patch.h>
#include <cy/build/service.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <sys/wait.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

using namespace cy;
using namespace cy::build;

namespace {

[[nodiscard]] Allocator& test_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

constexpr const char* kDescription =
    "cybuild 1\n"
    "node \"import:a\" import \"copy\"\n"
    "  source \"assets/a.txt\"\n"
    "  output \"derived/a.bin\"\n"
    "node \"import:b\" import \"copy\"\n"
    "  source \"assets/b.txt\"\n"
    "  bundle \"extra\"\n"
    "  output \"derived/b.bin\"\n"
    "node \"package\" package \"manifest\"\n"
    "  upstream \"import:a\"\n"
    "  upstream \"import:b\"\n"
    "  output \"derived/all.manifest\"\n";

/// A project that can be built at two revisions, so that a real patch exists between them.
class Fixture {
public:
    Fixture() : temp_("build-patch") {
        CY_REQUIRE(temp_.valid());
        CY_REQUIRE(assets::fs::create_directories(sources().c_str()).has_value());
        CY_REQUIRE(producers_.add_builtins().has_value());
        CY_REQUIRE(read_description(kDescription, graph_, &producers_).has_value());
        provider_ = std::make_unique<DirectorySourceProvider>(sources());
    }

    [[nodiscard]] std::string sources() const { return temp_.path() + "/project"; }
    [[nodiscard]] std::string artefacts() const { return temp_.path() + "/artefacts"; }
    [[nodiscard]] std::string install() const { return temp_.path() + "/install"; }

    void write(const char* name, std::string_view content) const {
        const std::string path = sources() + "/" + name;
        const usize slash = path.rfind('/');
        CY_REQUIRE(assets::fs::create_directories(path.substr(0, slash).c_str()).has_value());
        CY_REQUIRE(
            assets::fs::write_atomic(path.c_str(), content.data(), content.size()).has_value());
    }

    /// Build the current sources and package the result. The artefact store is shared between
    /// revisions on purpose: it is content-addressed, so the second build adds only what changed —
    /// which is what makes the patch between them small.
    [[nodiscard]] PackageSet build_revision(const char* revision) {
        BuildConfig config;
        config.graph = &graph_;
        config.producers = &producers_;
        config.sources = provider_.get();
        config.artefact_root = artefacts();
        config.cache.local = "";

        BuildService service;
        CY_REQUIRE(service.configure(config).has_value());
        const Expected<BuildReport, Error> report = service.build();
        CY_REQUIRE(report.has_value());
        CY_REQUIRE(report->succeeded());

        Provenance provenance;
        provenance.project = "tools/build/tests";
        provenance.revision = revision;
        const Expected<PackageSet, Error> packages =
            assemble(graph_, *report, std::move(provenance));
        CY_REQUIRE(packages.has_value());
        return *packages;
    }

private:
    cy::test::TempDir temp_;
    ProducerRegistry producers_;
    BuildGraph graph_;
    std::unique_ptr<DirectorySourceProvider> provider_;
};

[[nodiscard]] std::string read_installed(const Installation& installation, const char* name) {
    Array<u8> bytes(test_allocator());
    if (!installation.read(name, bytes)) {
        return {};
    }
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

/// Open the installation afresh and assert it still is the build it was, completely.
void check_intact(const std::string& root, const std::string& build_id, const char* content) {
    Installation installation;
    CY_REQUIRE(installation.open(root).has_value());
    const Expected<std::string, Error> current = installation.current_build();
    CY_REQUIRE(current.has_value());
    CY_CHECK_EQ(*current, build_id);
    CY_CHECK(installation.verify().has_value());
    CY_CHECK_EQ(read_installed(installation, "derived/a.bin"), content);
}

/// Apply a patch in a child process that is killed at `stage`. Answers the child's exit status.
///
/// `fork` with no `exec`: the child inherits the fixture exactly as the parent has it, which is
/// what makes this an interruption of THIS application rather than of a similar one. The child
/// never returns from `apply`, so nothing in it runs a destructor, and the parent then inspects the
/// installation the child left behind.
[[nodiscard]] int apply_and_die(const std::string& root, const PatchManifest& patch,
                                const ArtefactStore& store, PatchStage stage) {
    const pid_t child = ::fork();
    CY_REQUIRE(child >= 0);
    if (child == 0) {
        Installation installation;
        if (!installation.open(root)) {
            _exit(70);
        }
        const StoreChunkSource source(store);
        PatchInterrupt interrupt;
        interrupt.stage = stage;
        interrupt.hard = true;
        const Expected<PatchResult, Error> result = installation.apply(patch, source, interrupt);
        // Reaching here means the interruption did not fire, which is itself a failure the parent
        // must see rather than a silently successful patch.
        _exit(result && result->applied ? 71 : 72);
    }
    int status = 0;
    CY_REQUIRE_EQ(::waitpid(child, &status, 0), child);
    return status;
}

}  // namespace

CY_TEST_CASE("a build installs, and every chunk it names verifies") {
    Fixture fixture;
    fixture.write("assets/a.txt", "alpha one\n");
    fixture.write("assets/b.txt", "bravo\n");
    const PackageSet first = fixture.build_revision("r1");

    CY_REQUIRE_EQ(first.bundles.size(), 2U);
    CY_CHECK_EQ(first.bundles[0].name, "base");
    CY_CHECK_EQ(first.bundles[1].name, "extra");
    CY_CHECK_GT(first.size(), 0U);
    CY_CHECK(bundle_report(first).find("base") != std::string::npos);

    ArtefactStore store;
    CY_REQUIRE(store.configure(fixture.artefacts()).has_value());
    Installation installation;
    CY_REQUIRE(installation.open(fixture.install()).has_value());
    CY_REQUIRE(installation.install(first, store).has_value());

    const Expected<std::string, Error> current = installation.current_build();
    CY_REQUIRE(current.has_value());
    CY_CHECK_EQ(*current, first.build_id);
    CY_CHECK(installation.verify().has_value());
    CY_CHECK_EQ(read_installed(installation, "derived/a.bin"), "alpha one\n");
}

CY_TEST_CASE("a patch carries only what changed, applies atomically, and is refused twice") {
    Fixture fixture;
    fixture.write("assets/a.txt", "alpha one\n");
    fixture.write("assets/b.txt", "bravo\n");
    const PackageSet first = fixture.build_revision("r1");
    fixture.write("assets/a.txt", "alpha two\n");
    const PackageSet second = fixture.build_revision("r2");

    const PatchManifest patch = diff(first, second);
    // One asset changed, so the patch carries its chunk and the manifest chunk that names it —
    // never the bundle that contains them.
    CY_CHECK_EQ(patch.added.size(), 2U);
    CY_CHECK_LT(patch.transferred_bytes(), second.size());

    ArtefactStore store;
    CY_REQUIRE(store.configure(fixture.artefacts()).has_value());
    Installation installation;
    CY_REQUIRE(installation.open(fixture.install()).has_value());
    CY_REQUIRE(installation.install(first, store).has_value());

    const StoreChunkSource source(store);
    const Expected<PatchResult, Error> applied = installation.apply(patch, source);
    CY_REQUIRE(applied.has_value());
    CY_CHECK(applied->applied);
    CY_CHECK_EQ(applied->reached, PatchStage::Complete);
    check_intact(fixture.install(), second.build_id, "alpha two\n");

    // "WHEN a patch is offered THEN its manifest SHALL state which builds it applies to" — and a
    // statement nothing enforces is a comment. The same patch offered again is refused.
    CY_CHECK_FALSE(installation.apply(patch, source).has_value());
    check_intact(fixture.install(), second.build_id, "alpha two\n");
}

CY_TEST_CASE("an interrupted patch leaves the previous build intact and playable") {
    // The M6 exit criterion. Every stage before the switch, both softly and by killing the process.
    Fixture fixture;
    fixture.write("assets/a.txt", "alpha one\n");
    fixture.write("assets/b.txt", "bravo\n");
    const PackageSet first = fixture.build_revision("r1");
    fixture.write("assets/a.txt", "alpha two\n");
    const PackageSet second = fixture.build_revision("r2");
    const PatchManifest patch = diff(first, second);

    ArtefactStore store;
    CY_REQUIRE(store.configure(fixture.artefacts()).has_value());

    const PatchStage stages[] = {PatchStage::Begin, PatchStage::Fetch, PatchStage::Verify,
                                 PatchStage::Commit, PatchStage::Switch};
    for (const PatchStage stage : stages) {
        // Reinstall the first build, so every stage starts from the same place.
        (void)assets::fs::remove_directory_recursive(fixture.install().c_str());
        Installation installation;
        CY_REQUIRE(installation.open(fixture.install()).has_value());
        CY_REQUIRE(installation.install(first, store).has_value());

        CY_TEST_MESSAGE("interrupting at ", patch_stage_name(stage));

        // Softly: the call returns, having rolled back.
        {
            const StoreChunkSource source(store);
            PatchInterrupt interrupt;
            interrupt.stage = stage;
            const Expected<PatchResult, Error> result =
                installation.apply(patch, source, interrupt);
            CY_REQUIRE(result.has_value());
            CY_CHECK_FALSE(result->applied);
            CY_CHECK_EQ(result->reached, stage);
        }
        check_intact(fixture.install(), first.build_id, "alpha one\n");

        // Hard: the process is killed at that point. Nothing unwinds, nothing is cleaned up, and
        // the installation must still be the build it was.
        const int status = apply_and_die(fixture.install(), patch, store, stage);
        CY_REQUIRE(WIFEXITED(status));
        CY_CHECK_EQ(WEXITSTATUS(status), 97);
        check_intact(fixture.install(), first.build_id, "alpha one\n");

        // And the build still completes afterwards: a killed application leaves nothing that stops
        // the next one.
        const StoreChunkSource source(store);
        const Expected<PatchResult, Error> applied = installation.apply(patch, source);
        CY_REQUIRE(applied.has_value());
        CY_CHECK(applied->applied);
        check_intact(fixture.install(), second.build_id, "alpha two\n");
    }
}

namespace {

/// A chunk source that hands back the wrong bytes for one digest — a corrupted download.
class CorruptingSource final : public ChunkSource {
public:
    CorruptingSource(const ArtefactStore& store, assets::ContentHash corrupt)
        : inner_(store), corrupt_(corrupt) {}

    [[nodiscard]] Status fetch(const assets::ContentHash& digest, Array<u8>& out) const override {
        if (Status read = inner_.fetch(digest, out); !read) {
            return read;
        }
        if (digest == corrupt_) {
            if (Status pushed = out.push_back(static_cast<u8>('!')); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

private:
    StoreChunkSource inner_;
    assets::ContentHash corrupt_;
};

}  // namespace

CY_TEST_CASE("a corrupt download is caught, named, and installs nothing") {
    Fixture fixture;
    fixture.write("assets/a.txt", "alpha one\n");
    fixture.write("assets/b.txt", "bravo\n");
    const PackageSet first = fixture.build_revision("r1");
    fixture.write("assets/a.txt", "alpha two\n");
    const PackageSet second = fixture.build_revision("r2");
    const PatchManifest patch = diff(first, second);
    CY_REQUIRE_FALSE(patch.added.empty());

    ArtefactStore store;
    CY_REQUIRE(store.configure(fixture.artefacts()).has_value());
    Installation installation;
    CY_REQUIRE(installation.open(fixture.install()).has_value());
    CY_REQUIRE(installation.install(first, store).has_value());

    const CorruptingSource source(store, patch.added.front().digest);
    const Expected<PatchResult, Error> result = installation.apply(patch, source);
    CY_REQUIRE(result.has_value());
    CY_CHECK_FALSE(result->applied);
    CY_CHECK_EQ(result->reached, PatchStage::Verify);
    CY_CHECK_EQ(result->failed_chunk, patch.added.front().name);
    check_intact(fixture.install(), first.build_id, "alpha one\n");
}

CY_TEST_CASE("a chunk already installed under another name is not transferred again") {
    // Content addressing, doing the job a delta mechanism would otherwise have to: the patch's own
    // manifest lists a chunk, and the installation already holds those exact bytes.
    Fixture fixture;
    fixture.write("assets/a.txt", "identical\n");
    fixture.write("assets/b.txt", "identical\n");
    const PackageSet first = fixture.build_revision("r1");

    ArtefactStore store;
    CY_REQUIRE(store.configure(fixture.artefacts()).has_value());
    Installation installation;
    CY_REQUIRE(installation.open(fixture.install()).has_value());
    CY_REQUIRE(installation.install(first, store).has_value());

    // Two logical names, one digest, one file on disk.
    const std::vector<PackageEntry> entries = first.entries();
    CY_REQUIRE(entries.size() >= 2U);
    CY_CHECK_EQ(read_installed(installation, "derived/a.bin"),
                read_installed(installation, "derived/b.bin"));

    fixture.write("assets/b.txt", "changed\n");
    const PackageSet second = fixture.build_revision("r2");
    const PatchManifest patch = diff(first, second);
    const StoreChunkSource source(store);
    const Expected<PatchResult, Error> applied = installation.apply(patch, source);
    CY_REQUIRE(applied.has_value());
    CY_CHECK(applied->applied);
    // `derived/a.bin` did not change, so nothing about it was fetched.
    CY_CHECK_LE(applied->fetched_chunks, patch.added.size());
    check_intact(fixture.install(), second.build_id, "identical\n");
}
