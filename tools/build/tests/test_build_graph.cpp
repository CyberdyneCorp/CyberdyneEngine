// The derivation graph, the key model, and the three text formats. M6 tasks 7.1, 7.2 and 7.5.
//
// Unit rather than integration because nothing here touches a filesystem. What it asserts is the
// half of the milestone's exit criteria that is decided before anything runs: which contributions
// enter a key, that a prefix-free encoding cannot be made to collide, that a downstream key holds
// its upstream's OUTPUT digest, and that the graph answers "what does this change reach?" and "why
// is this in the build?" from the declarations alone.

#include <cy/build/description.h>
#include <cy/build/key.h>
#include <cy/build/package.h>
#include <cy/build/patch.h>
#include <cy/test/test.h>

#include <string>
#include <vector>

using namespace cy;
using namespace cy::build;

namespace {

[[nodiscard]] ToolchainFingerprint fingerprint(const char* flags = "-O2") {
    ToolchainFingerprint toolchain;
    toolchain.compiler = "GNU 13.3.0";
    toolchain.flags = flags;
    toolchain.standard_library = "libstdc++ 20240101";
    toolchain.target = "Linux x86_64";
    toolchain.libraries = "blake3=1.8.7;zstd=1.5.7";
    return toolchain;
}

[[nodiscard]] NodeDesc node(std::string name, std::vector<std::string> sources,
                            std::vector<std::string> upstreams) {
    NodeDesc desc;
    desc.name = std::move(name);
    desc.kind = NodeKind::Cook;
    desc.producer = "copy";
    desc.producer_version = 1;
    desc.sources = std::move(sources);
    desc.upstreams = std::move(upstreams);
    desc.outputs.push_back(desc.name + ".out");
    return desc;
}

[[nodiscard]] assets::ContentHash hash_of(std::string_view text) {
    return assets::content_hash(text.data(), text.size());
}

[[nodiscard]] assets::DerivationKey key_of(const NodeDesc& desc,
                                           const ToolchainFingerprint& toolchain,
                                           std::vector<KeyedDigest> sources = {},
                                           std::vector<KeyedDigest> upstreams = {}) {
    KeyInputs inputs;
    inputs.node = &desc;
    inputs.toolchain = &toolchain;
    inputs.sources = std::move(sources);
    inputs.upstreams = std::move(upstreams);
    const Expected<assets::DerivationKey, Error> key = derivation_key(inputs);
    CY_REQUIRE(key.has_value());
    return *key;
}

}  // namespace

CY_TEST_CASE("a graph orders its nodes after their upstreams and refuses a cycle") {
    BuildGraph graph;
    CY_REQUIRE(graph.add(node("c", {}, {"b"})).has_value());
    CY_REQUIRE(graph.add(node("a", {"in.txt"}, {})).has_value());
    CY_REQUIRE(graph.add(node("b", {}, {"a"})).has_value());
    CY_REQUIRE(graph.finalize().has_value());

    const std::vector<NodeId>& order = graph.order();
    CY_REQUIRE_EQ(order.size(), 3U);
    CY_CHECK_EQ(graph.node(order[0]).name, "a");
    CY_CHECK_EQ(graph.node(order[1]).name, "b");
    CY_CHECK_EQ(graph.node(order[2]).name, "c");

    BuildGraph cyclic;
    CY_REQUIRE(cyclic.add(node("x", {}, {"y"})).has_value());
    CY_REQUIRE(cyclic.add(node("y", {}, {"x"})).has_value());
    CY_CHECK_FALSE(cyclic.finalize().has_value());

    BuildGraph missing;
    CY_REQUIRE(missing.add(node("x", {}, {"nobody"})).has_value());
    CY_CHECK_FALSE(missing.finalize().has_value());
}

CY_TEST_CASE("a node's names must be project-relative, because an absolute path is machine-local") {
    // design.md §1.4, and E5: with absolute paths in the key every node with a file input misses
    // while early cutoff spares the nodes below them, so the shared cache dies silently.
    CY_CHECK(is_project_relative("assets/city.gltf"));
    CY_CHECK_FALSE(is_project_relative("/home/leonardo/assets/city.gltf"));
    CY_CHECK_FALSE(is_project_relative("C:/assets/city.gltf"));
    CY_CHECK_FALSE(is_project_relative("assets\\city.gltf"));
    CY_CHECK_FALSE(is_project_relative("../outside/city.gltf"));
    CY_CHECK(is_project_relative("assets/..city/x"));

    BuildGraph graph;
    CY_CHECK_FALSE(graph.add(node("a", {"/etc/passwd"}, {})).has_value());
}

CY_TEST_CASE(
    "a change reaches exactly its dependents, and a node explains why it is in the build") {
    BuildGraph graph;
    CY_REQUIRE(graph.add(node("import:a", {"a.txt"}, {})).has_value());
    CY_REQUIRE(graph.add(node("import:b", {"b.txt"}, {})).has_value());
    CY_REQUIRE(graph.add(node("cook", {}, {"import:a", "import:b"})).has_value());
    CY_REQUIRE(graph.add(node("package", {}, {"cook"})).has_value());
    CY_REQUIRE(graph.add(node("unrelated", {"c.txt"}, {})).has_value());
    CY_REQUIRE(graph.finalize().has_value());

    std::vector<std::string> reached;
    for (const NodeId id : graph.dependents_of_source("a.txt")) {
        reached.push_back(graph.node(id).name);
    }
    CY_REQUIRE_EQ(reached.size(), 3U);
    CY_CHECK_EQ(reached[0], "import:a");
    CY_CHECK_EQ(reached[1], "cook");
    CY_CHECK_EQ(reached[2], "package");

    // "Why is this in the build?" reads from a delivery root downward.
    const std::vector<NodeId> chain = graph.reference_chain(graph.find("import:a"));
    CY_REQUIRE_EQ(chain.size(), 3U);
    CY_CHECK_EQ(graph.node(chain[0]).name, "package");
    CY_CHECK_EQ(graph.node(chain[2]).name, "import:a");
}

CY_TEST_CASE("the toolchain is in every key, and a key without one is refused") {
    // design.md §1.3 — the correctness bug the spike was commissioned to find. Two builds of one
    // producer at -O2 and at -O0 computed the identical key, and one was served the other's
    // artefact with a reported cache hit.
    const NodeDesc desc = node("cook", {"a.txt"}, {});
    const std::vector<KeyedDigest> sources{{"a.txt", hash_of("content")}};

    const assets::DerivationKey optimised = key_of(desc, fingerprint("-O2"), sources);
    const assets::DerivationKey debug = key_of(desc, fingerprint("-O0"), sources);
    CY_CHECK_NE(optimised, debug);

    ToolchainFingerprint other = fingerprint();
    other.libraries = "blake3=1.8.7;zstd=1.5.5";
    CY_CHECK_NE(optimised, key_of(desc, other, sources));

    ToolchainFingerprint empty;
    KeyInputs inputs;
    inputs.node = &desc;
    inputs.toolchain = &empty;
    CY_CHECK_FALSE(derivation_key(inputs).has_value());
    inputs.toolchain = nullptr;
    CY_CHECK_FALSE(derivation_key(inputs).has_value());
}

CY_TEST_CASE("two option sets that collide under concatenation do not collide as a key") {
    // §1.2, E1: `{compress="ionlevel", ""="9"}` and `{compressi="on", "level"="9"}` hash to one
    // value under naive concatenation, and a cache keyed that way serves one cook's artefact to
    // another with no diagnostic at all.
    const ToolchainFingerprint toolchain = fingerprint();

    NodeDesc left = node("cook", {}, {});
    left.options.push_back(NodeOption{"compress", "ionlevel"});
    left.options.push_back(NodeOption{"", "9"});

    NodeDesc right = node("cook", {}, {});
    right.options.push_back(NodeOption{"compressi", "on"});
    right.options.push_back(NodeOption{"level", "9"});

    CY_CHECK_NE(key_of(left, toolchain), key_of(right, toolchain));

    // And the option order the caller happened to use must not matter, or a producer that discovers
    // its options in a different order misses its own cache (E10).
    BuildGraph graph;
    NodeDesc forward = node("cook", {}, {});
    forward.options.push_back(NodeOption{"a", "1"});
    forward.options.push_back(NodeOption{"b", "2"});
    NodeDesc reversed = node("cook2", {}, {});
    reversed.options.push_back(NodeOption{"b", "2"});
    reversed.options.push_back(NodeOption{"a", "1"});
    const NodeId first = *graph.add(std::move(forward));
    const NodeId second = *graph.add(std::move(reversed));
    CY_CHECK_EQ(graph.node(first).options[0].name, "a");
    CY_CHECK_EQ(graph.node(second).options[0].name, "a");
}

CY_TEST_CASE("a source's content decides the key, and its declared order does not") {
    const ToolchainFingerprint toolchain = fingerprint();
    const NodeDesc desc = node("cook", {"a.txt", "b.txt"}, {});

    const assets::DerivationKey forward =
        key_of(desc, toolchain, {{"a.txt", hash_of("A")}, {"b.txt", hash_of("B")}});
    const assets::DerivationKey reversed =
        key_of(desc, toolchain, {{"b.txt", hash_of("B")}, {"a.txt", hash_of("A")}});
    CY_CHECK_EQ(forward, reversed);

    const assets::DerivationKey changed =
        key_of(desc, toolchain, {{"a.txt", hash_of("A!")}, {"b.txt", hash_of("B")}});
    CY_CHECK_NE(forward, changed);
}

CY_TEST_CASE("a downstream key holds its upstream's output digest, so identical output cuts off") {
    // design.md §1.5, the decision that is expensive to reverse. Under deep input keys the
    // downstream key would move whenever the upstream's key moved; under output digests it moves
    // only when the upstream's BYTES move.
    const ToolchainFingerprint toolchain = fingerprint();
    const NodeDesc downstream = node("package", {}, {"cook"});

    const assets::ContentHash produced = hash_of("the same bytes");
    const assets::DerivationKey before = key_of(downstream, toolchain, {}, {{"cook", produced}});
    const assets::DerivationKey after = key_of(downstream, toolchain, {}, {{"cook", produced}});
    CY_CHECK_EQ(before, after);

    const assets::DerivationKey moved =
        key_of(downstream, toolchain, {}, {{"cook", hash_of("different bytes")}});
    CY_CHECK_NE(before, moved);

    // And the result digest itself is a function of names and content, never of order.
    const std::vector<std::pair<std::string, assets::ContentHash>> forward{{"one", hash_of("1")},
                                                                           {"two", hash_of("2")}};
    const std::vector<std::pair<std::string, assets::ContentHash>> reversed{{"two", hash_of("2")},
                                                                            {"one", hash_of("1")}};
    CY_CHECK_EQ(result_digest(forward), result_digest(reversed));
    CY_CHECK_NE(result_digest(forward),
                result_digest({{"one", hash_of("1")}, {"two", hash_of("3")}}));
}

CY_TEST_CASE("nothing that varies per machine or per run is in a key") {
    // §1.6, E6: with a build counter in the key, five identical builds produce zero hits and leave
    // forty cache entries where eight nodes' worth of content exists.
    const ToolchainFingerprint toolchain = fingerprint();
    NodeDesc desc = node("cook", {"a.txt"}, {});
    const std::vector<KeyedDigest> sources{{"a.txt", hash_of("content")}};
    const assets::DerivationKey baseline = key_of(desc, toolchain, sources);

    // The bundle a node's outputs land in is packaging policy, not derivation: it cannot change a
    // byte of the artefact, so it must not change the key.
    desc.bundle = "high-resolution-textures";
    CY_CHECK_EQ(baseline, key_of(desc, toolchain, sources));

    // Whether a node may run on a remote worker is scheduling, not content.
    desc.distributable = false;
    CY_CHECK_EQ(baseline, key_of(desc, toolchain, sources));

    // And the things that ARE content still move it.
    desc.profile = "dedicated-server";
    CY_CHECK_NE(baseline, key_of(desc, toolchain, sources));
}

CY_TEST_CASE("a build description round-trips") {
    const std::string document =
        "cybuild 1\n"
        "# a comment, and a blank line follow\n"
        "\n"
        "node \"import:a\" import \"copy\" 3\n"
        "  platform \"host\"\n"
        "  profile \"client\"\n"
        "  bundle \"base\"\n"
        "  source \"assets/a.txt\"\n"
        "  output \"derived/a.bin\"\n"
        "  option \"quality\" \"high\"\n"
        "node \"cook\" cook \"concat\" 1\n"
        "  upstream \"import:a\"\n"
        "  output \"derived/city.bin\"\n"
        "  distributable false\n";

    BuildGraph graph;
    CY_REQUIRE(read_description(document, graph).has_value());
    CY_REQUIRE_EQ(graph.size(), 2U);
    CY_CHECK_EQ(graph.node(graph.find("import:a")).producer_version, 3U);
    CY_CHECK_EQ(graph.node(graph.find("import:a")).option("quality"), "high");
    CY_CHECK_FALSE(graph.node(graph.find("cook")).distributable);

    BuildGraph again;
    CY_REQUIRE(read_description(write_description(graph), again).has_value());
    CY_CHECK_EQ(write_description(graph), write_description(again));

    BuildGraph rejected;
    CY_CHECK_FALSE(read_description("not a build file\n", rejected).has_value());
}

CY_TEST_CASE("a package manifest round-trips, and its build id follows its content") {
    PackageSet packages;
    packages.provenance.project = "samples/06-open-world";
    packages.provenance.toolchain = "abc";
    packages.bundles.push_back(
        Bundle{"base", {PackageEntry{"derived/a.bin", hash_of("A"), 1, "import:a"}}});
    packages.bundles.push_back(
        Bundle{"high", {PackageEntry{"derived/b.bin", hash_of("B"), 2, "import:b"}}});
    packages.build_id = "0000";

    const std::string document = write_package(packages);
    const Expected<PackageSet, Error> parsed = read_package(document);
    CY_REQUIRE(parsed.has_value());
    CY_CHECK_EQ(parsed->bundles.size(), 2U);
    CY_CHECK_EQ(parsed->provenance.project, "samples/06-open-world");
    CY_REQUIRE(parsed->bundle("high") != nullptr);
    CY_CHECK_EQ(parsed->bundle("high")->entries[0].name, "derived/b.bin");
    CY_CHECK_EQ(write_package(*parsed), document);
}

// ==================================================================================================
// M11.d task 7.5. `build-and-packaging` requires SEVEN things of a build's provenance and the
// manifest carried four: a build identity, the project revision, the platform and profile, and the
// toolchain digest. The engine revision, the plugin lockfile hash, the cook configuration and the
// toolchain VERSIONS were absent — so a shipped build could not say which engine tree it came from,
// which plugin set was locked, or which compiler a human should install to reproduce it.
//
// The case that matters is the round trip: a field that is written and not read is a field that
// vanishes the first time anything reads a manifest back, which is what `patch` and `install` do.
// ==================================================================================================
CY_TEST_CASE("a package manifest carries every provenance field the requirement names") {
    PackageSet packages;
    packages.provenance.project = "samples/11-ship";
    packages.provenance.revision = "project-abc123";
    packages.provenance.engine_revision = "engine-def456";
    packages.provenance.platform = "linux";
    packages.provenance.profile = "shipping";
    packages.provenance.cook_configuration = "textures=bc7 audio=vorbis";
    packages.provenance.lockfile = "lock-789";
    packages.provenance.toolchain = "digest-abc";
    packages.provenance.toolchain_versions = "clang 22.1.8; slang 2025.1";
    packages.bundles.push_back(
        Bundle{"base", {PackageEntry{"derived/a.bin", hash_of("A"), 1, "import:a"}}});
    packages.build_id = "0000";

    const std::string document = write_package(packages);
    const Expected<PackageSet, Error> parsed = read_package(document);
    CY_REQUIRE(parsed.has_value());

    // Each of the seven, named, so that a field dropped from the writer or the reader fails here
    // rather than in a bug report about a build nobody can reproduce.
    CY_CHECK_EQ(parsed->provenance.revision, "project-abc123");
    CY_CHECK_EQ(parsed->provenance.engine_revision, "engine-def456");
    CY_CHECK_EQ(parsed->provenance.lockfile, "lock-789");
    CY_CHECK_EQ(parsed->provenance.cook_configuration, "textures=bc7 audio=vorbis");
    CY_CHECK_EQ(parsed->provenance.toolchain, "digest-abc");
    CY_CHECK_EQ(parsed->provenance.toolchain_versions, "clang 22.1.8; slang 2025.1");
    // The build identity IS the content manifest hash — one field, because two would be two things
    // that can disagree.
    CY_CHECK_EQ(parsed->build_id, "0000");
    CY_CHECK_EQ(write_package(*parsed), document);

    // AND THE ENGINE AND PROJECT REVISIONS ARE NOT THE SAME FIELD. Before M11.d there was one, and
    // an engine built from a tag with a project built from a branch is the ordinary case.
    CY_CHECK(parsed->provenance.revision != parsed->provenance.engine_revision);
}

// ==================================================================================================
// M11.d task 7.4 — the content audit's COST half. `audit()` answers "why is this in the build?" and
// "what references this?" from the graph. `build-and-packaging` asks two more questions that
// nothing answered, and every number they need was already on the report: `NodeResult` carries the
// stage, the duration, the bytes and the outcome.
// ==================================================================================================
CY_TEST_CASE("time by stage separates a cache hit from work, and the rate follows") {
    // `node()` above gives a valid description — a node with no producer is REFUSED by add() — and
    // the kind is what these cases are about, so it is the only field overridden.
    BuildGraph graph;
    NodeDesc import_a = node("import:a", {}, {});
    import_a.kind = NodeKind::Import;
    NodeDesc import_b = node("import:b", {}, {});
    import_b.kind = NodeKind::Import;
    NodeDesc cook = node("cook:world", {}, {"import:a"});
    CY_REQUIRE(graph.add(import_a).has_value());
    CY_REQUIRE(graph.add(import_b).has_value());
    CY_REQUIRE(graph.add(cook).has_value());

    // Assigned rather than brace-initialised: `NodeResult` has twelve members and the build treats
    // a missing field initialiser as an error, so a designated-initialiser list here would have to
    // name every one of them and would break the day a thirteenth is added.
    const auto result = [](const char* name, NodeOutcome outcome, u64 duration, u64 bytes) {
        NodeResult node;
        node.name = name;
        node.outcome = outcome;
        node.duration_ns = duration;
        node.bytes_produced = bytes;
        return node;
    };

    BuildReport report;
    report.nodes.push_back(result("import:a", NodeOutcome::Cached, 1'000'000, 10));
    report.nodes.push_back(result("import:b", NodeOutcome::Ran, 3'000'000, 20));
    report.nodes.push_back(result("cook:world", NodeOutcome::Rebuilt, 5'000'000, 40));

    const std::vector<StageCost> stages = stage_costs(graph, report);
    CY_REQUIRE_EQ(stages.size(), 2U);
    // REQUIRE does not abort under -fno-exceptions, and every check below indexes `stages`. Without
    // this the real failure is buried under a page of out-of-bounds reads.
    if (stages.size() < 2) {
        return;
    }

    // Import: two nodes, one of them a cache hit, so fifty per cent.
    CY_CHECK(stages[0].kind == NodeKind::Import);
    CY_CHECK_EQ(stages[0].nodes, 2U);
    CY_CHECK_EQ(stages[0].cached, 1U);
    CY_CHECK_EQ(stages[0].rebuilt, 1U);
    CY_CHECK_EQ(stages[0].hit_rate_percent(), 50U);
    CY_CHECK_EQ(stages[0].work_ns, 4'000'000U);
    CY_CHECK_EQ(stages[0].bytes_produced, 30U);

    // Cook: one node, rebuilt, so no hits at all. `Ran` and `Rebuilt` are both work.
    CY_CHECK(stages[1].kind == NodeKind::Cook);
    CY_CHECK_EQ(stages[1].cached, 0U);
    CY_CHECK_EQ(stages[1].rebuilt, 1U);
    CY_CHECK_EQ(stages[1].hit_rate_percent(), 0U);

    // A stage nothing ran in is omitted rather than printed as a row of zeroes.
    for (const StageCost& stage : stages) {
        CY_CHECK_GT(stage.nodes, 0U);
    }

    const std::string text = stage_report(graph, report);
    CY_CHECK(text.find("import") != std::string::npos);
    CY_CHECK(text.find("50% hit") != std::string::npos);

    // A BUILD THAT RAN NOTHING SAYS SO. An empty table reads as a broken report, and "every output
    // was current" is a legitimate and common outcome.
    const BuildReport nothing;
    CY_CHECK(stage_costs(graph, nothing).empty());
    CY_CHECK(stage_report(graph, nothing).find("no node ran") != std::string::npos);
}

CY_TEST_CASE("size by category adds up to the package, and names what it cannot attribute") {
    BuildGraph graph;
    NodeDesc import_a = node("import:a", {}, {});
    import_a.kind = NodeKind::Import;
    NodeDesc shader = node("shader:lit", {}, {});
    shader.kind = NodeKind::Shader;
    CY_REQUIRE(graph.add(import_a).has_value());
    CY_REQUIRE(graph.add(shader).has_value());

    PackageSet packages;
    packages.bundles.push_back(
        Bundle{"base",
               {PackageEntry{"derived/a.bin", hash_of("A"), 100, "import:a"},
                PackageEntry{"derived/lit.spv", hash_of("L"), 30, "shader:lit"}}});
    packages.bundles.push_back(
        Bundle{"high", {PackageEntry{"derived/big.bin", hash_of("B"), 900, "import:a"}}});

    const std::vector<CategoryShare> shares = category_shares(graph, packages);
    CY_REQUIRE_EQ(shares.size(), 2U);
    if (shares.size() < 2) {
        return;
    }
    CY_CHECK(shares[0].kind == NodeKind::Import);
    CY_CHECK_EQ(shares[0].bytes, 1000U);
    CY_CHECK_EQ(shares[0].entries, 2U);
    // The "by asset" half of the same requirement: the largest single entry in the category.
    CY_CHECK_EQ(shares[0].largest, "derived/big.bin");
    CY_CHECK_EQ(shares[0].largest_bytes, 900U);
    CY_CHECK(shares[1].kind == NodeKind::Shader);
    CY_CHECK_EQ(shares[1].bytes, 30U);

    // THE SUM IS THE CHECK. A category report whose total differs from the package's own size has
    // lost bytes, and a reader who cannot see that cannot know.
    u64 accounted = 0;
    for (const CategoryShare& share : shares) {
        accounted += share.bytes;
    }
    CY_CHECK_EQ(accounted, packages.size());

    // An entry whose node has left the graph lands in `Unknown` rather than being dropped, so the
    // sum still adds up — which is what makes the sum a check rather than a decoration.
    packages.bundles[0].entries.push_back(
        PackageEntry{"derived/orphan.bin", hash_of("O"), 7, "gone:node"});
    const std::vector<CategoryShare> with_orphan = category_shares(graph, packages);
    u64 after = 0;
    for (const CategoryShare& share : with_orphan) {
        after += share.bytes;
    }
    CY_CHECK_EQ(after, packages.size());

    // And plugin and world region are NAMED as unreported rather than omitted: both need a
    // declaration `cybuild 1` does not carry, and an invented attribution in a size report is worse
    // than an absent one.
    const std::string text = content_report(graph, packages);
    CY_CHECK(text.find("size by category") != std::string::npos);
    CY_CHECK(text.find("size by plugin and by world region: NOT REPORTED") != std::string::npos);
}

CY_TEST_CASE("a patch carries only the chunks whose content changed") {
    PackageSet before;
    before.build_id = "before";
    before.bundles.push_back(
        Bundle{"base",
               {PackageEntry{"a", hash_of("A"), 1, "n"}, PackageEntry{"b", hash_of("B"), 1, "n"}}});
    PackageSet after = before;
    after.build_id = "after";
    after.bundles[0].entries[1].digest = hash_of("B2");

    const PatchManifest patch = diff(before, after);
    CY_CHECK_EQ(patch.produces, "after");
    CY_REQUIRE_EQ(patch.applies_to.size(), 1U);
    CY_CHECK_EQ(patch.applies_to[0], "before");
    CY_REQUIRE_EQ(patch.added.size(), 1U);
    CY_CHECK_EQ(patch.added[0].name, "b");
    CY_REQUIRE_EQ(patch.removed.size(), 1U);
    CY_CHECK_EQ(patch.removed[0].name, "b");

    const Expected<PatchManifest, Error> parsed = read_patch(write_patch(patch));
    CY_REQUIRE(parsed.has_value());
    CY_CHECK_EQ(parsed->produces, "after");
    CY_REQUIRE_EQ(parsed->added.size(), 1U);
    CY_CHECK_EQ(parsed->added[0].digest, patch.added[0].digest);
    // The target manifest travels inside the patch, so applying one needs no second fetch.
    CY_CHECK_EQ(parsed->target.build_id, after.build_id);
    CY_CHECK_EQ(write_package(parsed->target), write_package(after));

    // An unchanged build produces an empty patch, which is what makes `applies_to` meaningful.
    const PatchManifest nothing = diff(before, before);
    CY_CHECK(nothing.added.empty());
    CY_CHECK(nothing.removed.empty());
}

CY_TEST_CASE("the compiled-in toolchain fingerprint is complete") {
    // The generated header is produced from deps/manifest.toml and deps/host-tools.toml, and
    // tools/build/CMakeLists.txt refuses to emit an empty list. This is the runtime half of that:
    // a fingerprint that lost its content must fail a test rather than quietly key against nothing.
    const ToolchainFingerprint& toolchain = current_toolchain();
    CY_CHECK(toolchain_is_complete(toolchain));
    CY_CHECK(toolchain.libraries.find("blake3=") != std::string::npos);
    CY_CHECK(toolchain.libraries.find("zstd=") != std::string::npos);
    CY_CHECK(toolchain.libraries.find("rust=") != std::string::npos);
    CY_CHECK_FALSE(toolchain.compiler.empty());
    CY_CHECK_FALSE(toolchain.standard_library.empty());
    CY_CHECK_FALSE(toolchain.digest().is_zero());
}
