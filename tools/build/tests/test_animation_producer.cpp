// SPDX-License-Identifier: MIT
// The `animation` producer as a build-graph node: four import nodes over four FBX exports, and one
// character node over their bundles, cooking the skeleton, clips and program the runtime loads.
// Issue #76 stage 2.
//
// Only with CY_ANIMATION (tests/CMakeLists.txt): the producer is registered only where there is a
// runtime to cook for.

#include <cy/animation/cooked.h>
#include <cy/build/content_producers.h>
#include <cy/build/description.h>
#include <cy/build/service.h>
#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/fixtures.h>
#include <cy/test/test.h>

#include "fbx_clip_documents.h"

#include <memory>
#include <string>
#include <string_view>

using namespace cy;
using namespace cy::build;

namespace {

[[nodiscard]] Allocator& test_allocator() noexcept {
    return system_allocator(MemoryDomain::Animation);
}

constexpr const char* kDescription =
    "cybuild 1\n"
    "node \"import:idle\" import \"import\" 1\n"
    "  source \"animations/idle.fbx\"\n"
    "  output \"derived/idle.bundle\"\n"
    "node \"import:walk\" import \"import\" 1\n"
    "  source \"animations/walk.fbx\"\n"
    "  output \"derived/walk.bundle\"\n"
    "node \"import:run\" import \"import\" 1\n"
    "  source \"animations/run.fbx\"\n"
    "  output \"derived/run.bundle\"\n"
    "node \"import:die\" import \"import\" 1\n"
    "  source \"animations/die.fbx\"\n"
    "  output \"derived/die.bundle\"\n"
    "node \"animation:hero\" cook \"animation\" 1\n"
    "  source \"characters/hero.cyanim\"\n"
    "  upstream \"import:idle\"\n"
    "  upstream \"import:walk\"\n"
    "  upstream \"import:run\"\n"
    "  upstream \"import:die\"\n"
    "  output \"derived/hero.skeleton\"\n"
    "  output \"derived/hero.program\"\n"
    "  output \"derived/hero.idle.clip\"\n"
    "  output \"derived/hero.walk.clip\"\n"
    "  output \"derived/hero.run.clip\"\n"
    "  output \"derived/hero.die.clip\"\n";

constexpr const char* kCharacter =
    "cyanim 1\n"
    "name \"locomotion\"\n"
    "rig \"derived/walk.bundle\"\n"
    "clip \"idle\" \"derived/idle.bundle\"\n"
    "clip \"walk\" \"derived/walk.bundle\"\n"
    "clip \"run\" \"derived/run.bundle\"\n"
    "clip \"die\" \"derived/die.bundle\" hold\n"
    "blend \"walk_to_run\" 0.2\n";

class Project {
public:
    explicit Project(const char* label) : temp_(label) {
        CY_REQUIRE(temp_.valid());
        CY_REQUIRE(producers_.add_builtins().has_value());
        CY_REQUIRE(add_content_producers(producers_, nullptr).has_value());
        CY_REQUIRE(read_description(kDescription, graph_, &producers_).has_value());
        using cy::import::testing::animated_document;
        write("animations/idle.fbx", animated_document("idle", 10.0));
        write("animations/walk.fbx", animated_document("walk", 100.0));
        write("animations/run.fbx", animated_document("run", 200.0));
        // The death's rig stands 20 cm higher: congruent, another rest pose, so it is retargeted.
        write("animations/die.fbx", animated_document("die", 50.0, false, false, 20.0));
        write("characters/hero.cyanim", kCharacter);
    }

    void write(const std::string& name, std::string_view content) const {
        const std::string path = temp_.path() + "/project/" + name;
        const usize slash = path.rfind('/');
        CY_REQUIRE(assets::fs::create_directories(path.substr(0, slash).c_str()).has_value());
        CY_REQUIRE(
            assets::fs::write_atomic(path.c_str(), content.data(), content.size()).has_value());
    }

    [[nodiscard]] Expected<BuildReport, Error> build(BuildService& service) {
        provider_ = std::make_unique<DirectorySourceProvider>(temp_.path() + "/project");
        cache_root_ = temp_.path() + "/cache";
        BuildConfig config;
        config.graph = &graph_;
        config.producers = &producers_;
        config.sources = provider_.get();
        config.artefact_root = temp_.path() + "/artefacts";
        config.cache.local = cache_root_.c_str();
        config.workers = 0;
        CY_REQUIRE(service.configure(config).has_value());
        return service.build();
    }

private:
    cy::test::TempDir temp_;
    ProducerRegistry producers_;
    BuildGraph graph_;
    std::unique_ptr<DirectorySourceProvider> provider_;
    std::string cache_root_;
};

/// The bytes of one of the character node's outputs.
[[nodiscard]] Array<u8> output(const BuildReport& report, BuildService& service,
                               std::string_view name) {
    Array<u8> bytes(test_allocator());
    const NodeResult* character = report.node("animation:hero");
    CY_REQUIRE(character != nullptr);
    for (const NodeOutput& produced : character->outputs) {
        if (produced.name == name) {
            CY_REQUIRE(service.artefacts().get(produced.digest, bytes).has_value());
        }
    }
    CY_REQUIRE(!bytes.empty());
    return bytes;
}

}  // namespace

CY_TEST_CASE(
    "animation producer: four imports and a character description cook the records a game loads") {
    Project project("content-animation");
    BuildService cold;
    const Expected<BuildReport, Error> first = project.build(cold);
    CY_REQUIRE(first.has_value());
    CY_REQUIRE(first->succeeded());
    const NodeResult* character = first->node("animation:hero");
    CY_REQUIRE(character != nullptr);
    CY_CHECK(character->outcome == NodeOutcome::Ran);
    CY_CHECK_EQ(character->outputs.size(), usize{6});

    animation::Skeleton skeleton(test_allocator());
    animation::SkeletonProfile humanoid;
    const Array<u8> skeleton_bytes = output(*first, cold, "derived/hero.skeleton");
    CY_REQUIRE(animation::decode_skeleton(skeleton_bytes.span(), skeleton, humanoid).has_value());
    const Array<u8> program_bytes = output(*first, cold, "derived/hero.program");
    Expected<graph::pose::PoseProgram, Error> program =
        animation::decode_program(test_allocator(), program_bytes.span());
    CY_REQUIRE(program.has_value());
    CY_CHECK_EQ(program->clips().size(), usize{4});
    // The description's blend override reached the compiled machine.
    bool overridden = false;
    for (const graph::pose::Transition& transition : program->transitions()) {
        overridden = overridden || transition.duration == 0.2F;
    }
    CY_CHECK(overridden);

    const Array<u8> die_bytes = output(*first, cold, "derived/hero.die.clip");
    animation::Clip die(test_allocator());
    Array<Name> joints(test_allocator());
    CY_REQUIRE(animation::decode_clip(die_bytes.span(), die, joints).has_value());
    CY_CHECK(die.loop_mode() == animation::LoopMode::None);
    u16 mismatch = animation::kInvalidJoint;
    CY_CHECK(animation::clip_matches_skeleton(die, joints.span(), skeleton, mismatch));

    // Unchanged, the character is served from the store under the same key.
    BuildService warm;
    const Expected<BuildReport, Error> again = project.build(warm);
    CY_REQUIRE(again.has_value());
    CY_REQUIRE(again->node("animation:hero") != nullptr);
    CY_CHECK(again->node("animation:hero")->outcome == NodeOutcome::Cached);

    // A re-exported run re-imports it and re-cooks the character.
    project.write("animations/run.fbx", cy::import::testing::animated_document("run", 300.0));
    BuildService edited;
    const Expected<BuildReport, Error> after = project.build(edited);
    CY_REQUIRE(after.has_value());
    CY_REQUIRE(after->succeeded());
    CY_CHECK(after->node("animation:hero")->outcome != NodeOutcome::Cached);
    CY_CHECK(after->node("animation:hero")->key != character->key);
}

CY_TEST_CASE(
    "animation producer: a description naming a missing clip is refused with a diagnostic") {
    Project project("content-animation-refused");
    project.write("characters/hero.cyanim",
                  "cyanim 1\n"
                  "rig \"derived/walk.bundle\"\n"
                  "clip \"idle\" \"derived/idle.bundle\"\n"
                  "clip \"walk\" \"derived/walk.bundle\"\n"
                  "clip \"run\" \"derived/run.bundle\"\n");
    BuildService service;
    const Expected<BuildReport, Error> report = project.build(service);
    CY_REQUIRE(report.has_value());
    CY_CHECK_FALSE(report->succeeded());
    const NodeResult* character = report->node("animation:hero");
    CY_REQUIRE(character != nullptr);
    CY_CHECK(character->outcome == NodeOutcome::Failed);
    bool diagnosed = false;
    for (const Diagnostic& diagnostic : character->diagnostics) {
        diagnosed = diagnosed || diagnostic.code == "animation-cook";
    }
    CY_CHECK(diagnosed);
}
