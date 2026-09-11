// CyberML against the two things it has to live beside: the job system, and the determinism
// firewall. M8.c tasks 4.2 and 4.3.
//
// INTEGRATION, and deliberately so. Every case here starts a worker pool or builds an ECS world,
// and the unit tier's budget is a millisecond calibrated for machine speed, contention and the
// optimiser. A case that builds a world belongs here.
//
// ================================================================================================
// WHAT THIS FILE ADDS TO src/gameplay/tests/test_firewall.cpp AND DOES NOT DUPLICATE
// ================================================================================================
//
// That suite is the firewall's own, in both directions and with a negative control, and its
// inference direction drives `WriteOrigin::Inference` by opening the scope by hand. What it cannot
// do is name a `cy::ml::InferenceSession`, because src/gameplay/ does not depend on CyberML.
//
// So what is proved here is the JOIN: that a session decides its own origin from what its model
// asset declares and what configuration it is running, and that `ResultScope` carries that decision
// to the ECS write path. That is where "pinned" stops being a claim in a file and becomes the one
// bit `origin_may_write_authoritative` reads.

#include <cy/test/test.h>

#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/firewall.h>
#include <cy/ecs/world.h>
#include <cy/ml/schedule.h>
#include <cy/ml/session.h>

#include <cstddef>

#include "fake_backend.h"

namespace {

using namespace cy;
using namespace cy::ml;
using cy::ml::test::FakeBackend;
using cy::ml::test::make_fake_asset;

// --- The world half -------------------------------------------------------------------------

/// Replicated, so the firewall guards it. `vfx-system` names "network-replicated values" first in
/// the list of things a restricted origin may not be a source of truth for, and `ml-inference` says
/// the same thing from its own side about a non-pinned model.
struct Threat {
    i32 level = 0;
};

/// Presentation: `Persistence(Derived)` declares nothing authoritative, so the firewall derives
/// nothing and the write is permitted. Its presence is what stops the test from proving only that
/// everything is refused.
struct Highlight {
    f32 intensity = 0.0F;
};

[[nodiscard]] reflect::FieldInfo make_field(const char* name, u32 id, reflect::FieldKind kind,
                                            u32 offset, u32 size,
                                            reflect::FieldAttributes attributes) noexcept {
    reflect::FieldInfo field;
    field.name = name;
    field.id = reflect::FieldId(id);
    field.kind = kind;
    field.offset = offset;
    field.size = size;
    field.attributes = attributes;
    return field;
}

[[nodiscard]] const reflect::TypeInfo& threat_type() noexcept {
    static reflect::FieldAttributes replicated = [] {
        reflect::FieldAttributes attributes;
        attributes.declared = reflect::AttributeKind::Replicated;
        attributes.replicated = reflect::ReplicatedAttribute{"quantised", "bits=16", "owner"};
        return attributes;
    }();
    static const reflect::FieldInfo fields[] = {
        make_field("level", 9611, reflect::FieldKind::I32,
                   static_cast<u32>(offsetof(Threat, level)), static_cast<u32>(sizeof(i32)),
                   replicated),
    };
    static reflect::TypeInfo info;
    info.name = "cy::ml::test::Threat";
    info.id = reflect::TypeId(9610);
    info.size = static_cast<u32>(sizeof(Threat));
    info.alignment = static_cast<u32>(alignof(Threat));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 1;
    return info;
}

[[nodiscard]] const reflect::TypeInfo& highlight_type() noexcept {
    static reflect::FieldAttributes derived = [] {
        reflect::FieldAttributes attributes;
        attributes.declared = reflect::AttributeKind::Persistence;
        attributes.persistence = reflect::PersistenceKind::Derived;
        return attributes;
    }();
    static const reflect::FieldInfo fields[] = {
        make_field("intensity", 9621, reflect::FieldKind::F32,
                   static_cast<u32>(offsetof(Highlight, intensity)), static_cast<u32>(sizeof(f32)),
                   derived),
    };
    static reflect::TypeInfo info;
    info.name = "cy::ml::test::Highlight";
    info.id = reflect::TypeId(9620);
    info.size = static_cast<u32>(sizeof(Highlight));
    info.alignment = static_cast<u32>(alignof(Highlight));
    info.trivially_relocatable = true;
    info.fields = fields;
    info.field_count = 1;
    return info;
}

/// A model asset that is pinned to the configuration `desc` will run.
[[nodiscard]] Expected<ModelAsset, Error> pinned_asset(Allocator& allocator,
                                                       const SessionDesc& desc) {
    Expected<ModelAsset, Error> asset = make_fake_asset(allocator);
    if (!asset) {
        return asset;
    }
    ModelDeterminism determinism;
    determinism.backend = BackendKind::OnnxRuntime;
    determinism.precision = asset.value().precision();
    determinism.verified_configuration =
        configuration_digest(BackendKind::OnnxRuntime, asset.value().precision(), desc.device);
    asset.value().set_determinism(determinism);
    return asset;
}

struct World {
    ecs::World world;
    ecs::ComponentTypeId threat = ecs::kInvalidComponent;
    ecs::ComponentTypeId highlight = ecs::kInvalidComponent;
    ecs::Entity agent;
    ecs::AuthorityDerivationReport derivation;

    World() noexcept : world(system_allocator(MemoryDomain::Ecs)) {}

    [[nodiscard]] bool build() noexcept {
        if (!world.initialize().has_value()) {
            return false;
        }
        auto threat_id = world.components().register_reflected(threat_type());
        auto highlight_id = world.components().register_reflected(highlight_type());
        if (!threat_id.has_value() || !highlight_id.has_value()) {
            return false;
        }
        threat = *threat_id;
        highlight = *highlight_id;
        if (!world.firewall().declare_from_reflection(world.components(), derivation).has_value()) {
            return false;
        }
        const ecs::ComponentTypeId set[] = {threat, highlight};
        auto created = world.create(Span<const ecs::ComponentTypeId>(set, 2));
        if (!created.has_value()) {
            return false;
        }
        agent = *created;
        return true;
    }
};

CY_TEST_CASE("ml.runtime: a session over a non-pinned model cannot write replicated state") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend backend(BackendKind::OnnxRuntime);
    BackendRegistry registry;
    CY_REQUIRE(registry.add(&backend).has_value());
    Expected<ModelAsset, Error> asset = make_fake_asset(allocator);
    CY_REQUIRE(asset.has_value());

    SessionDesc desc;
    SelectionReport report;
    Expected<InferenceSession, Error> session =
        InferenceSession::create(allocator, registry, asset.value(), desc, report);
    CY_REQUIRE(session.has_value());
    CY_CHECK_FALSE(session.value().is_pinned());
    CY_CHECK_EQ(static_cast<u32>(session.value().write_origin()),
                static_cast<u32>(ecs::WriteOrigin::Inference));

    World fixture;
    CY_REQUIRE(fixture.build());
    CY_REQUIRE(fixture.world.firewall().guards(fixture.threat));

    CY_CHECK(session.value().run().has_value());
    const u64 before = fixture.world.firewall().refusals();
    {
        // The shape a game writes: open the session's origin and act on the result.
        const ResultScope scope(session.value(), "ai.threat-classifier");

        auto* level = fixture.world.get_mut<Threat>(fixture.agent, fixture.threat);
        // REFUSED. Not "returns a pointer the caller should not use" — there is no pointer.
        CY_CHECK_EQ(level, nullptr);

        // And the presentation write in the same scope is permitted, which is what stops this from
        // being a test that everything is refused.
        auto* highlight = fixture.world.get_mut<Highlight>(fixture.agent, fixture.highlight);
        CY_REQUIRE_NE(highlight, nullptr);
        Tensor* scores = session.value().output(0);
        CY_REQUIRE_NE(scores, nullptr);
        const Span<const f32> values = scores->as<const f32>();
        CY_REQUIRE_FALSE(values.empty());
        highlight->intensity = values[0];
    }
    CY_CHECK_EQ(fixture.world.firewall().refusals(), before + 1);
    CY_CHECK_EQ(fixture.world.firewall().refusals_by(ecs::WriteOrigin::Inference), 1U);
    // The development-build diagnostic names the writer, and the writer is the string the session's
    // consumer passed rather than "inference".
    CY_CHECK_EQ(static_cast<u32>(fixture.world.firewall().last_violation().origin),
                static_cast<u32>(ecs::WriteOrigin::Inference));
}

CY_TEST_CASE("ml.runtime: a session pinned to the configuration it runs may write") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend backend(BackendKind::OnnxRuntime);
    BackendRegistry registry;
    CY_REQUIRE(registry.add(&backend).has_value());

    SessionDesc desc;
    Expected<ModelAsset, Error> asset = pinned_asset(allocator, desc);
    CY_REQUIRE(asset.has_value());
    SelectionReport report;
    Expected<InferenceSession, Error> session =
        InferenceSession::create(allocator, registry, asset.value(), desc, report);
    CY_REQUIRE(session.has_value());
    CY_CHECK(session.value().is_pinned());
    CY_CHECK_EQ(static_cast<u32>(session.value().write_origin()),
                static_cast<u32>(ecs::WriteOrigin::PinnedInference));

    World fixture;
    CY_REQUIRE(fixture.build());
    {
        const ResultScope scope(session.value(), "ai.threat-classifier");
        auto* level = fixture.world.get_mut<Threat>(fixture.agent, fixture.threat);
        CY_REQUIRE_NE(level, nullptr);
        level->level = 7;
    }
    CY_CHECK_EQ(fixture.world.firewall().refusals(), 0U);
    CY_CHECK_EQ(fixture.world.get<Threat>(fixture.agent, fixture.threat)->level, 7);
}

CY_TEST_CASE("ml.runtime: the same asset on another device is not pinned") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend backend(BackendKind::OnnxRuntime);
    BackendRegistry registry;
    CY_REQUIRE(registry.add(&backend).has_value());

    SessionDesc verified;
    Expected<ModelAsset, Error> asset = pinned_asset(allocator, verified);
    CY_REQUIRE(asset.has_value());

    // The asset is verified on "cpu"; this session runs on "cuda:0". Same model, same backend, same
    // precision — and `ml-inference` treats inference as non-deterministic across DEVICES, so this
    // session is not pinned and its result may not reach authoritative state.
    SessionDesc elsewhere;
    elsewhere.device = "cuda:0";
    SelectionReport report;
    Expected<InferenceSession, Error> session =
        InferenceSession::create(allocator, registry, asset.value(), elsewhere, report);
    CY_REQUIRE(session.has_value());
    CY_CHECK_FALSE(session.value().is_pinned());
    CY_CHECK_NE(session.value().configuration(),
                asset.value().determinism().verified_configuration);
}

// --- The job-system half --------------------------------------------------------------------

CY_TEST_CASE("ml.runtime: the scheduler dispatches onto the job system and completes on pump") {
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend backend(BackendKind::OnnxRuntime);
    BackendRegistry registry;
    CY_REQUIRE(registry.add(&backend).has_value());
    Expected<ModelAsset, Error> asset = make_fake_asset(allocator);
    CY_REQUIRE(asset.has_value());

    jobs::JobSystem system;
    jobs::JobSystemConfig config;
    config.worker_count = 4;
    CY_REQUIRE(system.start(config).has_value());

    Array<InferenceSession> sessions(allocator);
    CY_REQUIRE(sessions.reserve(8).has_value());
    for (u32 index = 0; index < 8; ++index) {
        SessionDesc desc;
        SelectionReport report;
        Expected<InferenceSession, Error> session =
            InferenceSession::create(allocator, registry, asset.value(), desc, report);
        CY_REQUIRE(session.has_value());
        CY_REQUIRE(sessions.push_back(std::move(session).value()).has_value());
    }

    InferenceScheduler scheduler;
    scheduler.set_job_system(&system);
    scheduler.set_budget(InferenceBudget{0, 0});
    scheduler.begin_frame();
    for (InferenceSession& session : sessions.span()) {
        CY_REQUIRE(scheduler.submit(session, InferencePriority::Normal).has_value());
    }
    CY_REQUIRE(scheduler.dispatch().has_value());
    CY_CHECK_EQ(scheduler.pump(), 8U);
    for (const InferenceSession& session : sessions.span()) {
        CY_CHECK_EQ(session.stats().invocations, 1U);
        CY_CHECK(scheduler.result(session).fresh);
    }
    system.shutdown();
}

CY_TEST_CASE("ml.runtime: a scheduler destroyed with work in flight waits for it") {
    // TEARDOWN UNDER LOAD. A job holds a pointer to its request for as long as it runs, so a
    // scheduler that went out of scope between `dispatch` and `pump` would leave a worker reading
    // freed memory. The destructor waits, and this case is what makes that a checked property
    // rather than a comment — under a sanitizer build it is the one that would report.
    Allocator& allocator = system_allocator(MemoryDomain::Engine);
    FakeBackend backend(BackendKind::OnnxRuntime);
    BackendRegistry registry;
    CY_REQUIRE(registry.add(&backend).has_value());
    Expected<ModelAsset, Error> asset = make_fake_asset(allocator);
    CY_REQUIRE(asset.has_value());

    jobs::JobSystem system;
    jobs::JobSystemConfig config;
    config.worker_count = 4;
    CY_REQUIRE(system.start(config).has_value());

    Array<InferenceSession> sessions(allocator);
    CY_REQUIRE(sessions.reserve(32).has_value());
    for (u32 index = 0; index < 32; ++index) {
        SessionDesc desc;
        desc.max_batch = 64;
        SelectionReport report;
        Expected<InferenceSession, Error> session =
            InferenceSession::create(allocator, registry, asset.value(), desc, report);
        CY_REQUIRE(session.has_value());
        CY_REQUIRE(sessions.push_back(std::move(session).value()).has_value());
    }

    {
        InferenceScheduler scheduler;
        scheduler.set_job_system(&system);
        scheduler.set_budget(InferenceBudget{0, 0});
        scheduler.begin_frame();
        for (InferenceSession& session : sessions.span()) {
            CY_REQUIRE(scheduler.submit(session, InferencePriority::Normal).has_value());
        }
        CY_REQUIRE(scheduler.dispatch().has_value());
        // No pump. The scheduler goes out of scope here with thirty-two jobs in flight.
    }
    system.shutdown();

    // Every request that was dispatched ran exactly once: the destructor waited rather than
    // abandoning them.
    for (const InferenceSession& session : sessions.span()) {
        CY_CHECK_EQ(session.stats().invocations, 1U);
    }
}

}  // namespace
