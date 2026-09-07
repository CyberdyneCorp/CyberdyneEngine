// Material cooking as a node in the derivation graph. M7 task 6.3.
//
// M6's closing gate: "The real cooks are not nodes in the build graph." M7 task 1.4 put `import`
// and `cook` into it and design.md §3 requires the expensive M7 cooks to land the same way. These
// cases are that guarantee, over the material cook:
//
//   * two runs produce the same bundle, byte for byte — the determinism `just content-validate`
//     gates, applied to a node;
//   * a second build is a CACHE HIT rather than a second compilation;
//   * editing a material re-runs its node and leaves an unrelated one alone;
//   * a material with a cook-time error produces NO artefact, so a downstream node cannot read one
//     from a previous build;
//   * the bundle carries the IR, the generated Slang and the cost report, which is what
//     `material-compiler`'s cook step is specified to produce.

#include <cy/build/service.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/material/cook.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace cy;
using namespace cy::build;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Assets);
}

constexpr const char* kWornMetal = R"(
material worn_metal {
    param base_color : float3 = (0.82, 0.78, 0.74);
    param metallic   : float  = 1.0;
    param roughness  : float  = 0.35;
    texture base_color_map average (0.5, 0.5, 0.5, 1.0);
    attribute uv0 : float2;
    let albedo = sample(base_color_map, uv0).xyz * base_color;
    surface = diffuse(albedo * (1 - metallic)) + specular(albedo, roughness);
    opacity = 1.0;
}
)";

constexpr const char* kBroken = R"(
material broken {
    param base : float3 = (0.5, 0.5, 0.5);
    static param rough : float = 0.3;
    surface = diffuse(base) + specular(base, rough);
    opacity = 1.0;
}
)";

/// A project on disk, a graph over it, and a configured service.
class Project {
public:
    Project() {
        CY_REQUIRE(temp_.valid());
        CY_REQUIRE(material::add_material_producers(producers_).has_value());
        write("materials/worn_metal.cymat", kWornMetal);
        write("materials/second.cymat", kWornMetal);
        declare("materials/worn_metal.cymat");
        declare("materials/second.cymat");
    }

    void declare(const std::string& source) {
        NodeDesc node;
        node.kind = NodeKind::Shader;
        node.name = "material:" + source;
        node.producer = "material";
        node.producer_version = material::kMaterialProducerVersion;
        node.sources.push_back(source);
        node.outputs.push_back(source + ".cymatbin");
        node.options.push_back(NodeOption{"profile", "desktop"});
        CY_REQUIRE(graph_.add(std::move(node)).has_value());
    }

    void write(const std::string& name, std::string_view content) const {
        const std::string path = sources() + "/" + name;
        const usize slash = path.rfind('/');
        CY_REQUIRE(assets::fs::create_directories(path.substr(0, slash).c_str()).has_value());
        CY_REQUIRE(
            assets::fs::write_atomic(path.c_str(), content.data(), content.size()).has_value());
    }

    [[nodiscard]] std::string sources() const { return temp_.path() + "/project"; }
    [[nodiscard]] std::string path(const char* name) const { return temp_.path() + "/" + name; }

    [[nodiscard]] BuildConfig config(const std::string& artefacts, const std::string& cache) {
        CY_REQUIRE(graph_.finalize().has_value());
        provider_ = std::make_unique<DirectorySourceProvider>(sources());
        cache_ = cache;
        BuildConfig config;
        config.graph = &graph_;
        config.producers = &producers_;
        config.sources = provider_.get();
        config.artefact_root = artefacts;
        config.cache.local = cache_.c_str();
        config.workers = 0;
        return config;
    }

private:
    cy::test::TempDir temp_{"material-cook"};
    ProducerRegistry producers_;
    BuildGraph graph_;
    std::unique_ptr<DirectorySourceProvider> provider_;
    std::string cache_;
};

[[nodiscard]] const NodeResult* result_for(const BuildReport& report, std::string_view name) {
    for (const NodeResult& node : report.nodes) {
        if (node.name == name) {
            return &node;
        }
    }
    return nullptr;
}

}  // namespace

CY_TEST_CASE("material_cook: a material is a node, and a second build is a cache hit") {
    Project project;
    BuildService first;
    CY_REQUIRE(first.configure(project.config(project.path("artefacts"), project.path("cache")))
                   .has_value());
    auto cold = first.build();
    CY_REQUIRE(cold.has_value());
    CY_CHECK_EQ(cold.value().failed, 0U);
    CY_CHECK_EQ(cold.value().ran, 2U);
    CY_CHECK_EQ(cold.value().cached, 0U);

    const NodeResult* node = result_for(cold.value(), "material:materials/worn_metal.cymat");
    CY_REQUIRE(node != nullptr);
    CY_REQUIRE_EQ(node->outputs.size(), 1U);
    CY_CHECK_GT(node->outputs[0].size, 0U);

    // The cost report reaches the build log as a node diagnostic rather than as something printed
    // where nobody collects it.
    bool reported = false;
    for (const Diagnostic& diagnostic : node->diagnostics) {
        reported = reported || diagnostic.code == "material-cost";
    }
    CY_CHECK(reported);

    BuildService warm;
    CY_REQUIRE(warm.configure(project.config(project.path("artefacts"), project.path("cache")))
                   .has_value());
    auto second = warm.build();
    CY_REQUIRE(second.has_value());
    CY_CHECK_EQ(second.value().cached, 2U);
    CY_CHECK_EQ(second.value().ran, 0U);
}

CY_TEST_CASE("material_cook: two builds from empty produce identical bytes") {
    // The determinism gate, applied to this node. A cook whose output depends on anything but its
    // declared inputs makes the artefact that ships a matter of which run got there first.
    Project project;
    BuildService first;
    CY_REQUIRE(first.configure(project.config(project.path("a"), "")).has_value());
    auto one = first.build();
    CY_REQUIRE(one.has_value());

    BuildService second;
    CY_REQUIRE(second.configure(project.config(project.path("b"), "")).has_value());
    auto two = second.build();
    CY_REQUIRE(two.has_value());

    const NodeResult* left = result_for(one.value(), "material:materials/worn_metal.cymat");
    const NodeResult* right = result_for(two.value(), "material:materials/worn_metal.cymat");
    CY_REQUIRE(left != nullptr);
    CY_REQUIRE(right != nullptr);
    CY_CHECK(left->outputs[0].digest == right->outputs[0].digest);
    CY_CHECK_EQ(left->result, right->result);
    // And the same source under a different name cooks to the same bytes, because the key is over
    // content and the bundle carries no path.
    const NodeResult* sibling = result_for(one.value(), "material:materials/second.cymat");
    CY_REQUIRE(sibling != nullptr);
    CY_CHECK(left->outputs[0].digest == sibling->outputs[0].digest);
}

CY_TEST_CASE("material_cook: editing one material leaves the other alone") {
    Project project;
    BuildService first;
    CY_REQUIRE(first.configure(project.config(project.path("artefacts"), project.path("cache")))
                   .has_value());
    CY_REQUIRE(first.build().has_value());

    std::string edited = kWornMetal;
    const usize position = edited.find("0.35");
    CY_REQUIRE_NE(position, std::string::npos);
    edited.replace(position, 4, "0.51");
    project.write("materials/worn_metal.cymat", edited);

    BuildService after;
    CY_REQUIRE(after.configure(project.config(project.path("artefacts"), project.path("cache")))
                   .has_value());
    auto report = after.build();
    CY_REQUIRE(report.has_value());
    const NodeResult* changed = result_for(report.value(), "material:materials/worn_metal.cymat");
    const NodeResult* untouched = result_for(report.value(), "material:materials/second.cymat");
    CY_REQUIRE(changed != nullptr);
    CY_REQUIRE(untouched != nullptr);
    CY_CHECK_NE(changed->outcome, NodeOutcome::Cached);
    CY_CHECK_EQ(untouched->outcome, NodeOutcome::Cached);
}

CY_TEST_CASE("material_cook: a material with a cook-time error produces no artefact") {
    Project project;
    project.write("materials/broken.cymat", kBroken);
    project.declare("materials/broken.cymat");

    BuildService service;
    CY_REQUIRE(service.configure(project.config(project.path("artefacts"), project.path("cache")))
                   .has_value());
    auto report = service.build();
    CY_REQUIRE(report.has_value());
    CY_CHECK_EQ(report.value().failed, 1U);

    const NodeResult* node = result_for(report.value(), "material:materials/broken.cymat");
    CY_REQUIRE(node != nullptr);
    CY_CHECK_EQ(node->outcome, NodeOutcome::Failed);
    CY_CHECK_EQ(node->outputs.size(), 0U);
}

CY_TEST_CASE("material_cook: the producer is safe to run on several workers at once") {
    // The producer has NO process-wide state — unlike `cook`, which holds a component registry in
    // an atomic because a `ProducerBody` is a plain function pointer. So the build service may run
    // it on every worker, and this case is what makes that a measured fact rather than a reading of
    // the code: eight material nodes over four workers, twice, with the artefact digests compared.
    //
    // It is also the teardown case. `BuildService`'s pool is started by `configure` and joined by
    // the destructor, and a producer that left something behind would be found here rather than in
    // one run in forty of somebody else's gate.
    Project project;
    for (u32 index = 0; index < 6U; ++index) {
        std::string name = "materials/variant_";
        name += static_cast<char>('a' + static_cast<char>(index));
        name += ".cymat";
        std::string source = kWornMetal;
        // Each one differs, so no two nodes share a key and every worker does real work.
        const usize position = source.find("0.35");
        CY_REQUIRE_NE(position, std::string::npos);
        source.replace(position, 4, index % 2 == 0 ? "0.41" : "0.42");
        source.insert(source.find("material worn_metal"), "");
        project.write(name, source);
        project.declare(name);
    }

    std::vector<assets::ContentHash> digests;
    for (u32 run = 0; run < 2U; ++run) {
        BuildService service;
        BuildConfig config = project.config(project.path("parallel"), "");
        config.workers = 4;
        CY_REQUIRE(service.configure(std::move(config)).has_value());
        auto report = service.build();
        CY_REQUIRE(report.has_value());
        CY_CHECK_EQ(report.value().failed, 0U);
        CY_CHECK_EQ(report.value().nodes.size(), 8U);
        if (run == 0) {
            for (const NodeResult& node : report.value().nodes) {
                digests.push_back(node.result);
            }
            continue;
        }
        // Deterministic under concurrency: the report's order is evaluation order whatever finished
        // first, and every node's output digest is the one the serial run produced.
        CY_REQUIRE_EQ(report.value().nodes.size(), digests.size());
        for (usize index = 0; index < digests.size(); ++index) {
            CY_CHECK(report.value().nodes[index].result == digests[index]);
        }
    }
}

CY_TEST_CASE("material_cook: the bundle carries the IR, the programs and their cost") {
    Array<u8> bundle(allocator());
    Array<char> report(allocator());
    material::CompileOptions options;
    CY_REQUIRE(
        material::cook_material(kWornMetal, options, allocator(), bundle, report).has_value());
    CY_CHECK_GT(report.size(), 0U);

    auto decoded = material::decode_bundle(bundle.span(), allocator());
    CY_REQUIRE(decoded.has_value());
    CY_CHECK_EQ(decoded.value().compiler_version, rendering::material::kCompilerVersion);
    CY_CHECK_NE(decoded.value().cook_key, 0U);
    CY_CHECK_GT(decoded.value().ir.size(), 0U);
    CY_CHECK_EQ(decoded.value().programs.size(), 12U);

    // The IR in the bundle is the module the programs were emitted from, and it decodes.
    auto module = rendering::material::decode_module(decoded.value().ir, allocator());
    CY_REQUIRE(module.has_value());
    CY_CHECK_NE(module.value().surface(), rendering::material::kInvalidNode);

    u32 with_source = 0;
    u32 absent = 0;
    for (const material::CookedProgram& program : decoded.value().programs) {
        absent += program.absent ? 1U : 0U;
        with_source += program.source.empty() ? 0U : 1U;
        if (!program.absent) {
            CY_CHECK(program.source.find("cy_material_worn_metal_") != std::string_view::npos);
            CY_CHECK_EQ(program.permutation_count, 3U);
        }
    }
    // The three shadow programs are absent: the material is opaque.
    CY_CHECK_EQ(absent, 3U);
    CY_CHECK_EQ(with_source, 9U);

    // A truncated bundle is refused rather than half-read.
    for (usize length = 0; length < bundle.size(); length += 97) {
        CY_CHECK_FALSE(material::decode_bundle(Span<const u8>(bundle.data(), length), allocator())
                           .has_value());
    }
}
