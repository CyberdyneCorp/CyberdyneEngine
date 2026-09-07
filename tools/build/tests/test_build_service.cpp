// The build service, and both of M6's build exit criteria. Tasks 7.2, 7.3 and 7.4.
//
// Integration, because every case here writes real files, runs real workers and destroys them —
// which is the point. The two criteria the roadmap states are asserted directly rather than through
// a proxy:
//
//   * "A one-asset change invalidates only the derivations that depend on it."
//   * "A cold build and a cache-warm build produce byte-identical artefacts."
//
// Beside them are the four properties that make those two mean something: an undeclared read is a
// defect, a mutated artefact is reported rather than served, cancellation preserves the work
// already done, and a service destroyed while its workers are still inside it tears down cleanly.

#include <cy/build/description.h>
#include <cy/build/package.h>
#include <cy/build/service.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace cy;
using namespace cy::build;

namespace {

[[nodiscard]] Allocator& test_allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

/// A project on disk: a source directory, an artefact store and a cache, all under one scratch
/// root.
///
/// Held together because the three are what a build IS, and because the interesting assertions are
/// about what survives between two of them.
class Project {
public:
    explicit Project(const char* label) : temp_(label) {
        CY_REQUIRE(temp_.valid());
        CY_REQUIRE(assets::fs::create_directories(sources().c_str()).has_value());
        CY_REQUIRE(producers_.add_builtins().has_value());
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

    [[nodiscard]] ProducerRegistry& producers() noexcept { return producers_; }
    [[nodiscard]] BuildGraph& graph() noexcept { return graph_; }

    void describe(std::string_view document) {
        CY_REQUIRE(read_description(document, graph_, &producers_).has_value());
    }

    /// Configure a service over this project. `cache_root` empty disables the cache entirely, which
    /// is the clean-build configuration.
    [[nodiscard]] BuildConfig config(const std::string& artefact_root,
                                     const std::string& cache_root, u32 workers = 0) {
        provider_ = std::make_unique<DirectorySourceProvider>(sources());
        cache_root_ = cache_root;
        BuildConfig config;
        config.graph = &graph_;
        config.producers = &producers_;
        config.sources = provider_.get();
        config.artefact_root = artefact_root;
        config.cache.local = cache_root_.c_str();
        config.workers = workers;
        return config;
    }

private:
    cy::test::TempDir temp_;
    ProducerRegistry producers_;
    BuildGraph graph_;
    std::unique_ptr<DirectorySourceProvider> provider_;
    std::string cache_root_;
};

/// Every artefact a report names, by node and output, as bytes. What "byte-identical" is asserted
/// over: comparing digests alone would be asserting that BLAKE3 works.
[[nodiscard]] std::vector<std::string> artefact_bytes(const BuildReport& report,
                                                      const ArtefactStore& store) {
    std::vector<std::string> bytes;
    Array<u8> buffer(test_allocator());
    for (const NodeResult& result : report.nodes) {
        for (const NodeOutput& output : result.outputs) {
            CY_REQUIRE(store.get(output.digest, buffer).has_value());
            bytes.emplace_back(reinterpret_cast<const char*>(buffer.data()), buffer.size());
        }
    }
    return bytes;
}

[[nodiscard]] std::vector<std::string> ran_nodes(const BuildReport& report) {
    std::vector<std::string> names;
    for (const NodeResult& result : report.nodes) {
        if (result.outcome == NodeOutcome::Ran || result.outcome == NodeOutcome::Rebuilt) {
            names.push_back(result.name);
        }
    }
    return names;
}

/// The graph both criteria are measured on: two independent imports, a cook over both, a package
/// over the cook, and an unrelated pair beside them that must never move.
constexpr const char* kDescription =
    "cybuild 1\n"
    "node \"import:a\" import \"normalise\"\n"
    "  source \"assets/a.txt\"\n"
    "  output \"derived/a.bin\"\n"
    "node \"import:b\" import \"normalise\"\n"
    "  source \"assets/b.txt\"\n"
    "  output \"derived/b.bin\"\n"
    "node \"cook\" cook \"concat\"\n"
    "  upstream \"import:a\"\n"
    "  upstream \"import:b\"\n"
    "  output \"derived/city.bin\"\n"
    "node \"package\" package \"manifest\"\n"
    "  upstream \"cook\"\n"
    "  output \"derived/city.manifest\"\n"
    "node \"import:c\" import \"copy\"\n"
    "  source \"assets/c.txt\"\n"
    "  bundle \"extra\"\n"
    "  output \"derived/c.bin\"\n"
    "node \"cook:c\" cook \"copy\"\n"
    "  upstream \"import:c\"\n"
    "  bundle \"extra\"\n"
    "  output \"derived/c.cooked\"\n";

void seed(const Project& project) {
    project.write("assets/a.txt", "alpha\n");
    project.write("assets/b.txt", "bravo\n");
    project.write("assets/c.txt", "charlie\n");
}

}  // namespace

CY_TEST_CASE("a cold build and a cache-warm build produce byte-identical artefacts") {
    // M6 exit criterion. Three builds: cold, warm over the same cache, and a second cold build into
    // a completely separate pair of directories. All three must agree byte for byte.
    Project project("build-identical");
    seed(project);
    project.describe(kDescription);

    BuildService cold;
    CY_REQUIRE(cold.configure(project.config(project.path("cold"), "")).has_value());
    const Expected<BuildReport, Error> first = cold.build();
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(first->succeeded());
    CY_CHECK_EQ(first->ran, 6U);
    CY_CHECK_EQ(first->cached, 0U);
    const std::vector<std::string> cold_bytes = artefact_bytes(*first, cold.artefacts());

    BuildService warming;
    CY_REQUIRE(warming.configure(project.config(project.artefacts(), project.cache())).has_value());
    CY_REQUIRE(warming.build().has_value());

    BuildService warm;
    CY_REQUIRE(warm.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> second = warm.build();
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(second->succeeded());
    CY_CHECK_EQ(second->cached, 6U);
    CY_CHECK_EQ(second->ran, 0U);
    CY_CHECK_EQ(artefact_bytes(*second, warm.artefacts()), cold_bytes);

    BuildService again;
    CY_REQUIRE(again.configure(project.config(project.path("cold2"), "")).has_value());
    const Expected<BuildReport, Error> third = again.build();
    CY_REQUIRE(third.has_value());
    CY_CHECK_EQ(artefact_bytes(*third, again.artefacts()), cold_bytes);

    // And the whole product agrees, not only artefact by artefact: two builds have one build id.
    Provenance provenance;
    provenance.project = "test";
    const Expected<PackageSet, Error> from_cold = assemble(project.graph(), *first, provenance);
    const Expected<PackageSet, Error> from_warm = assemble(project.graph(), *second, provenance);
    CY_REQUIRE(from_cold.has_value());
    CY_REQUIRE(from_warm.has_value());
    CY_CHECK_EQ(from_cold->build_id, from_warm->build_id);
    CY_CHECK_EQ(write_package(*from_cold), write_package(*from_warm));
}

CY_TEST_CASE("a one-asset change invalidates exactly its dependents") {
    // M6's other exit criterion, and the one a key that is too coarse or too fine both fail.
    Project project("build-invalidate");
    seed(project);
    project.describe(kDescription);

    BuildService first;
    CY_REQUIRE(first.configure(project.config(project.artefacts(), project.cache())).has_value());
    CY_REQUIRE(first.build().has_value());

    CY_TEST_SUBCASE("a change to one asset reaches its dependents and nothing else") {
        project.write("assets/a.txt", "alpha changed\n");
        BuildService second;
        CY_REQUIRE(
            second.configure(project.config(project.artefacts(), project.cache())).has_value());
        const Expected<BuildReport, Error> report = second.build();
        CY_REQUIRE(report.has_value());
        CY_REQUIRE(report->succeeded());

        const std::vector<std::string> ran = ran_nodes(*report);
        CY_REQUIRE_EQ(ran.size(), 3U);
        CY_CHECK_EQ(ran[0], "import:a");
        CY_CHECK_EQ(ran[1], "cook");
        CY_CHECK_EQ(ran[2], "package");
        CY_CHECK_EQ(report->cached, 3U);
    }

    CY_TEST_SUBCASE("rewriting a file with identical content rebuilds nothing") {
        // "Invalidation SHALL be content-based, not timestamp-based." The write below changes the
        // modification time and nothing else.
        project.write("assets/a.txt", "alpha\n");
        BuildService second;
        CY_REQUIRE(
            second.configure(project.config(project.artefacts(), project.cache())).has_value());
        const Expected<BuildReport, Error> report = second.build();
        CY_REQUIRE(report.has_value());
        CY_CHECK_EQ(report->cached, 6U);
        CY_CHECK_EQ(report->ran, 0U);
    }

    CY_TEST_SUBCASE("an edit the producer normalises away stops at the node that normalised it") {
        // design.md §1.5 and E7, as a regression test. `normalise` strips trailing whitespace, so
        // this edit changes import:a's INPUT and not its OUTPUT. Under deep input keys three nodes
        // would rebuild; under upstream output digests exactly one does.
        project.write("assets/a.txt", "alpha   \n");
        BuildService second;
        CY_REQUIRE(
            second.configure(project.config(project.artefacts(), project.cache())).has_value());
        const Expected<BuildReport, Error> report = second.build();
        CY_REQUIRE(report.has_value());
        CY_REQUIRE(report->succeeded());

        const std::vector<std::string> ran = ran_nodes(*report);
        CY_REQUIRE_EQ(ran.size(), 1U);
        CY_CHECK_EQ(ran[0], "import:a");
        CY_CHECK_EQ(report->cached, 5U);
    }

    CY_TEST_SUBCASE("a producer version increase invalidates every node invoking it") {
        // "WHEN a cooker's version increases THEN the keys of every node invoking it SHALL change."
        Project bumped("build-version");
        seed(bumped);
        bumped.describe(
            "cybuild 1\n"
            "node \"import:a\" import \"normalise\" 2\n"
            "  source \"assets/a.txt\"\n"
            "  output \"derived/a.bin\"\n");
        BuildService service;
        CY_REQUIRE(
            service.configure(bumped.config(project.artefacts(), project.cache())).has_value());
        const Expected<BuildReport, Error> report = service.build();
        CY_REQUIRE(report.has_value());
        CY_CHECK_EQ(report->ran, 1U);
    }
}

CY_TEST_CASE("deleting the derived data cache loses nothing") {
    // "WHEN the derived data cache is deleted entirely THEN the next build SHALL regenerate it, and
    // no project content SHALL be lost."
    Project project("build-disposable");
    seed(project);
    project.describe(kDescription);

    BuildService first;
    CY_REQUIRE(first.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> before = first.build();
    CY_REQUIRE(before.has_value());

    CY_REQUIRE(assets::fs::remove_directory_recursive(project.cache().c_str()).has_value());
    CY_REQUIRE(assets::fs::remove_directory_recursive(project.artefacts().c_str()).has_value());

    BuildService second;
    CY_REQUIRE(second.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> after = second.build();
    CY_REQUIRE(after.has_value());
    CY_REQUIRE(after->succeeded());
    CY_CHECK_EQ(after->ran, 6U);
    CY_CHECK_EQ(artefact_bytes(*after, second.artefacts()),
                artefact_bytes(*before, first.artefacts()));
    // Every source is still there: the cache held nothing authoritative.
    CY_CHECK(assets::fs::exists((project.sources() + "/assets/a.txt").c_str()));
}

CY_TEST_CASE("an artefact mutated in place is reported rather than served") {
    // M6's adversarial pass: "mutate an immutable artefact". A downstream key holds an upstream's
    // output digest, so an artefact that changed under its digest would make every key computed
    // from it a lie — and nothing would report it, because the key still matches.
    Project project("build-immutable");
    seed(project);
    project.describe(kDescription);

    BuildService service;
    CY_REQUIRE(service.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> report = service.build();
    CY_REQUIRE(report.has_value());

    const NodeResult* cooked = report->node("cook");
    CY_REQUIRE(cooked != nullptr);
    CY_REQUIRE_EQ(cooked->outputs.size(), 1U);
    const assets::ContentHash digest = cooked->outputs[0].digest;
    const std::string path = service.artefacts().path_of(digest);

    // The store makes an artefact read-only, so this is what it takes to break the rule at all.
    CY_REQUIRE_EQ(::chmod(path.c_str(), 0644), 0);
    const char kTampered[] = "tampered";
    CY_REQUIRE(
        assets::fs::write_atomic(path.c_str(), kTampered, sizeof(kTampered) - 1).has_value());

    CY_CHECK_FALSE(service.artefacts().verify(digest).has_value());
    Array<u8> bytes(test_allocator());
    CY_CHECK_FALSE(service.artefacts().get(digest, bytes).has_value());

    u64 corrupt = 0;
    const Expected<u64, Error> checked = service.artefacts().verify_all(&corrupt);
    CY_REQUIRE(checked.has_value());
    CY_CHECK_EQ(corrupt, 1U);
    CY_CHECK_GT(*checked, corrupt);

    // And a build that has to READ it fails rather than serving it. The cache is cleared first so
    // that the downstream node genuinely reads the bytes: a cache hit is answered from the key and
    // never touches the artefact, which is why `verify_all` above exists as a separate audit and
    // why `build-and-packaging` asks for one.
    CY_REQUIRE(assets::fs::remove_directory_recursive(project.cache().c_str()).has_value());
    BuildService again;
    CY_REQUIRE(again.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> second = again.build();
    CY_REQUIRE(second.has_value());
    const NodeResult* downstream = second->node("package");
    CY_REQUIRE(downstream != nullptr);
    CY_CHECK_EQ(downstream->outcome, NodeOutcome::Failed);
    CY_CHECK_FALSE(second->succeeded());
}

namespace {

/// A producer that reads a file its node did not declare. The defect no key can catch — E9 shows
/// the build serving a stale artefact and reporting success — so it has to be caught here.
[[nodiscard]] Status produce_undeclared(NodeContext& context) {
    Array<u8> bytes(test_allocator());
    if (Status read = context.read("assets/secret.txt", bytes); !read) {
        return read;
    }
    return context.write(context.node().outputs.front(), bytes.data(), bytes.size());
}

/// A producer that reads a file it discovered, recording the dependency as it reads. The legitimate
/// counterpart: a shader's include, a glTF material's texture.
[[nodiscard]] Status produce_discovering(NodeContext& context) {
    Array<u8> bytes(test_allocator());
    if (Status read = context.discover("assets/include.txt", bytes); !read) {
        return read;
    }
    return context.write(context.node().outputs.front(), bytes.data(), bytes.size());
}

[[nodiscard]] Status produce_failing(NodeContext& context) {
    context.diagnose(Severity::Error, "deliberate", "this producer always fails");
    return make_unexpected(Error{ErrorCode::Internal, "deliberate failure", 0});
}

}  // namespace

CY_TEST_CASE("an undeclared read is a defect, and a discovered one is not") {
    Project project("build-undeclared");
    seed(project);
    project.write("assets/secret.txt", "not declared\n");
    project.write("assets/include.txt", "included\n");
    CY_REQUIRE(
        project.producers().add(Producer{"undeclared", 1, produce_undeclared, true}).has_value());
    CY_REQUIRE(
        project.producers().add(Producer{"discovering", 1, produce_discovering, true}).has_value());
    project.describe(
        "cybuild 1\n"
        "node \"sneaky\" cook \"undeclared\"\n"
        "  output \"derived/sneaky.bin\"\n"
        "node \"honest\" cook \"discovering\"\n"
        "  output \"derived/honest.bin\"\n");

    BuildService service;
    CY_REQUIRE(service.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> report = service.build();
    CY_REQUIRE(report.has_value());

    CY_REQUIRE_EQ(report->violations.size(), 1U);
    CY_CHECK_EQ(report->violations[0].kind, ViolationKind::UndeclaredRead);
    CY_CHECK_EQ(report->violations[0].node, "sneaky");
    CY_CHECK_EQ(report->violations[0].name, "assets/secret.txt");
    CY_CHECK_EQ(report->node("sneaky")->outcome, NodeOutcome::Failed);
    CY_CHECK_EQ(report->node("honest")->outcome, NodeOutcome::Ran);

    // The discovered dependency is re-checked at lookup: changing it invalidates the entry and
    // names it, which is the outcome `asset-import-pipeline` asks a report to carry.
    project.write("assets/include.txt", "included, differently\n");
    BuildService again;
    CY_REQUIRE(again.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> second = again.build();
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(second->node("honest")->outcome, NodeOutcome::Rebuilt);
    CY_CHECK(second->node("honest")->reason.find("assets/include.txt") != std::string::npos);
}

CY_TEST_CASE("a failing node poisons its dependents and leaves its siblings alone") {
    Project project("build-failure");
    seed(project);
    CY_REQUIRE(project.producers().add(Producer{"failing", 1, produce_failing, true}).has_value());
    project.describe(
        "cybuild 1\n"
        "node \"bad\" cook \"failing\"\n"
        "  output \"derived/bad.bin\"\n"
        "node \"downstream\" cook \"copy\"\n"
        "  upstream \"bad\"\n"
        "  output \"derived/downstream.bin\"\n"
        "node \"sibling\" import \"copy\"\n"
        "  source \"assets/a.txt\"\n"
        "  output \"derived/sibling.bin\"\n");

    BuildService service;
    CY_REQUIRE(service.configure(project.config(project.artefacts(), project.cache())).has_value());
    const Expected<BuildReport, Error> report = service.build();
    CY_REQUIRE(report.has_value());
    CY_CHECK_FALSE(report->succeeded());
    CY_CHECK_EQ(report->node("bad")->outcome, NodeOutcome::Failed);
    CY_CHECK_EQ(report->node("downstream")->outcome, NodeOutcome::Skipped);
    CY_CHECK_EQ(report->node("sibling")->outcome, NodeOutcome::Ran);

    // "Validation SHALL run before packaging": a failed build produces no package.
    CY_CHECK_FALSE(assemble(project.graph(), *report, Provenance{}).has_value());
}

namespace {

std::atomic<u32> g_in_flight{0};
std::atomic<u32> g_peak_in_flight{0};
std::atomic<u32> g_started{0};

/// A producer that takes measurable time and reports how many of it ran at once.
[[nodiscard]] Status produce_slow(NodeContext& context) {
    g_started.fetch_add(1, std::memory_order_relaxed);
    const u32 concurrent = g_in_flight.fetch_add(1, std::memory_order_relaxed) + 1;
    u32 peak = g_peak_in_flight.load(std::memory_order_relaxed);
    while (concurrent > peak &&
           !g_peak_in_flight.compare_exchange_weak(peak, concurrent, std::memory_order_relaxed)) {
    }
    for (u32 step = 0; step < 40 && !context.is_cancelled(); ++step) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    g_in_flight.fetch_sub(1, std::memory_order_relaxed);

    const std::string content(context.node().name);
    return context.write(context.node().outputs.front(), content.data(), content.size());
}

[[nodiscard]] std::string wide_description(u32 nodes) {
    std::string document = "cybuild 1\n";
    for (u32 index = 0; index < nodes; ++index) {
        const std::string name = "slow" + std::to_string(index);
        document += "node \"" + name + "\" cook \"slow\"\n";
        document += "  output \"derived/" + name + ".bin\"\n";
    }
    return document;
}

}  // namespace

CY_TEST_CASE("independent nodes run in parallel, and the result does not depend on how many did") {
    // The producer is registered BEFORE the description is read: `read_description` resolves a
    // node's producer version from the registry when the line omits it.
    Project project("build-parallel");
    CY_REQUIRE(project.producers().add(Producer{"slow", 1, produce_slow, true}).has_value());
    project.describe(wide_description(8));

    g_peak_in_flight.store(0);
    BuildService parallel;
    CY_REQUIRE(parallel.configure(project.config(project.path("parallel"), "", 4)).has_value());
    const Expected<BuildReport, Error> concurrent = parallel.build();
    CY_REQUIRE(concurrent.has_value());
    CY_REQUIRE(concurrent->succeeded());
    CY_CHECK_GT(g_peak_in_flight.load(), 1U);

    BuildService single;
    CY_REQUIRE(single.configure(project.config(project.path("single"), "", 0)).has_value());
    const Expected<BuildReport, Error> sequential = single.build();
    CY_REQUIRE(sequential.has_value());
    CY_CHECK_EQ(artefact_bytes(*concurrent, parallel.artefacts()),
                artefact_bytes(*sequential, single.artefacts()));
    // Reports are in evaluation order whichever worker finished first, so they are comparable.
    for (usize index = 0; index < concurrent->nodes.size(); ++index) {
        CY_CHECK_EQ(concurrent->nodes[index].name, sequential->nodes[index].name);
        CY_CHECK_EQ(concurrent->nodes[index].result, sequential->nodes[index].result);
    }
}

CY_TEST_CASE("cancelling a build preserves the artefacts already produced") {
    // "WHEN a build is cancelled midway THEN artefacts already produced SHALL remain in the cache
    // and SHALL not be rebuilt next time."
    Project project("build-cancel");
    CY_REQUIRE(project.producers().add(Producer{"slow", 1, produce_slow, true}).has_value());
    project.describe(wide_description(8));

    g_started.store(0);
    BuildService service;
    CY_REQUIRE(
        service.configure(project.config(project.artefacts(), project.cache(), 2)).has_value());

    std::thread canceller([&service] {
        while (g_started.load(std::memory_order_relaxed) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        service.cancel();
    });
    const Expected<BuildReport, Error> report = service.build();
    canceller.join();
    CY_REQUIRE(report.has_value());
    CY_CHECK(report->cancelled);
    CY_CHECK_GT(report->skipped, 0U);
    const u64 produced = report->ran;

    // The second build re-runs only what the first did not finish.
    BuildService resumed;
    CY_REQUIRE(
        resumed.configure(project.config(project.artefacts(), project.cache(), 2)).has_value());
    const Expected<BuildReport, Error> second = resumed.build();
    CY_REQUIRE(second.has_value());
    CY_REQUIRE(second->succeeded());
    CY_CHECK_EQ(second->cached, produced);
    CY_CHECK_EQ(second->ran + second->cached, 8U);
}

CY_TEST_CASE("a build service destroyed while its workers are running tears down cleanly") {
    // Hard rule: teardown under load, not only steady state. M5.5's gate found this project's first
    // real engine defect by destroying a subsystem while a worker was still inside it — one run in
    // forty. M6 creates and destroys build services continuously, so this loop does exactly that:
    // the destructor runs on this thread while `build()` is still executing on another.
    for (u32 iteration = 0; iteration < 24; ++iteration) {
        Project project("build-teardown");
        CY_REQUIRE(project.producers().add(Producer{"slow", 1, produce_slow, true}).has_value());
        project.describe(wide_description(6));

        g_started.store(0);
        auto service = std::make_unique<BuildService>();
        CY_REQUIRE(service->configure(project.config(project.artefacts(), project.cache(), 4))
                       .has_value());

        BuildService* raw = service.get();
        std::atomic<bool> finished{false};
        std::thread runner([raw, &finished] {
            const Expected<BuildReport, Error> report = raw->build();
            finished.store(report.has_value(), std::memory_order_relaxed);
        });

        // Wait until the build is genuinely in flight. Destroying before `build()` had incremented
        // its in-flight counter would be a race in the TEST rather than in the service.
        while (g_started.load(std::memory_order_relaxed) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        service->cancel();
        service.reset();  // the destructor waits for the build to leave, then joins the pool
        runner.join();
        CY_CHECK(finished.load());
    }
}

CY_TEST_CASE("the graph explains an invalidation without building anything") {
    // "WHEN a node runs unexpectedly THEN the tooling SHALL show which input changed and which
    // nodes depend on it."
    Project project("build-explain");
    seed(project);
    project.describe(kDescription);

    BuildService service;
    CY_REQUIRE(service.configure(project.config(project.artefacts(), project.cache())).has_value());
    const std::vector<std::string> reached = service.would_invalidate("assets/b.txt");
    CY_REQUIRE_EQ(reached.size(), 3U);
    CY_CHECK_EQ(reached[0], "import:b");
    CY_CHECK_EQ(reached[2], "package");
    CY_CHECK(service.would_invalidate("assets/nothing.txt").empty());
}

namespace {

/// What a client of the build service sees. `build-and-packaging` requires a structured protocol —
/// "job started, progress, diagnostic, artefact ready, job completed" — and forbids the alternative
/// in as many words: it "SHALL NOT be shell invocation with output parsing".
struct EventLog {
    std::atomic<u32> started{0};
    std::atomic<u32> completed{0};
    std::atomic<u32> artefacts{0};
    std::atomic<u32> diagnostics{0};
    std::atomic<u32> builds{0};
};

void record_event(void* user, const BuildEvent& event) {
    EventLog& log = *static_cast<EventLog*>(user);
    switch (event.kind) {
        case BuildEventKind::JobStarted:
            log.started.fetch_add(1);
            break;
        case BuildEventKind::JobCompleted:
            log.completed.fetch_add(1);
            break;
        case BuildEventKind::ArtefactReady:
            log.artefacts.fetch_add(1);
            break;
        case BuildEventKind::Diagnostic:
            log.diagnostics.fetch_add(1);
            break;
        case BuildEventKind::BuildCompleted:
            log.builds.fetch_add(1);
            break;
        case BuildEventKind::Progress:
            break;
    }
}

}  // namespace

CY_TEST_CASE("the service speaks a structured protocol, and degrades to local with no workers") {
    Project project("build-events");
    seed(project);
    project.describe(kDescription);

    EventLog log;
    BuildConfig config = project.config(project.artefacts(), project.cache(), 2);
    config.sink = record_event;
    config.sink_user = &log;
    // Distribution is asked for and is not available. "WHEN no remote workers are reachable THEN
    // the build SHALL execute locally with no change in result."
    config.distributed = true;

    BuildService service;
    CY_REQUIRE(service.configure(std::move(config)).has_value());
    const Expected<BuildReport, Error> report = service.build();
    CY_REQUIRE(report.has_value());
    CY_REQUIRE(report->succeeded());

    CY_CHECK_EQ(log.started.load(), 6U);
    CY_CHECK_EQ(log.completed.load(), 6U);
    CY_CHECK_EQ(log.artefacts.load(), 6U);
    CY_CHECK_EQ(log.builds.load(), 1U);
    // One diagnostic, and it is the degradation being announced rather than hidden.
    CY_CHECK_EQ(log.diagnostics.load(), 1U);
    CY_CHECK_EQ(report->distributed_nodes, 0U);

    // And the result is what a local build produces: the same artefacts, byte for byte.
    BuildService local;
    CY_REQUIRE(local.configure(project.config(project.path("local"), "")).has_value());
    const Expected<BuildReport, Error> plain = local.build();
    CY_REQUIRE(plain.has_value());
    CY_CHECK_EQ(artefact_bytes(*plain, local.artefacts()),
                artefact_bytes(*report, service.artefacts()));
}
