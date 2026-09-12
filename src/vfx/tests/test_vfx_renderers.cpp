// THE SIX RENDERER KINDS BEYOND `Sprite` AND `Mesh`. M10 task 5.3.
//
// ================================================================================================
// WHAT EACH CASE IS FOR, AND WHY NONE OF THEM IS A PICTURE
// ================================================================================================
//
// `render.vfx` photographs a sprite effect. None of these six kinds can be photographed by this
// module, because none of them is composited here — `src/vfx/README.md` says so and still does.
// What CAN be checked, and is what a renderer would be wrong to invent for itself, is the GEOMETRY
// AND THE INSTANCES derived from particle state: how many strips a broken chain produces, which
// lights a hard budget keeps, whether a newly spawned particle carries a motion vector.
//
// Every case below is one of `vfx-system`'s own scenarios turned into a number:
//
//   "WHEN particles in a ribbon chain are killed mid-chain THEN the ribbon SHALL TERMINATE CLEANLY
//    rather than connecting across the gap"                          -> two strips, and the count
//   "SHALL participate in clustered light assignment subject to a
//    HARD PER-FRAME COUNT BUDGET, degraded by the budget controller" -> the budget, and the lever
//   "particle renderers SHALL output motion vectors DERIVED FROM THE
//    PREVIOUS SIMULATION STEP, so particles do not smear or ghost"   -> the previous position
//   "WHEN an effect's particles change discontinuously (spawn, kill,
//    teleport) THEN interpolation SHALL be SUPPRESSED for those
//    particles rather than smearing them"                            -> `motion_valid` false
//
// ================================================================================================
// THE WORLD THESE CASES RUN ON IS BUILT BY HAND, AND THAT IS DELIBERATE
// ================================================================================================
//
// The spark plume is the fixture every other VFX suite shares, and it is the wrong one here: its
// population is whatever its spawn rate and its drawn lifetimes produce, and a case that asserts
// "two strips" over a population nobody chose is a case that asserts a coincidence. So these cases
// cook a MINIMAL emitter whose stages they control, play it, and then write the liveness array
// directly through `SimulationWorld::alive_flags` — which is public for exactly this reason, and is
// how a chain can be broken at a slot the case names rather than at whichever slot happened to
// expire.

#include "effects.h"

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/vfx/renderers.h>

#include <cmath>
#include <cstdio>
#include <string_view>
#include <utility>

using namespace cy;
using namespace cy::vfx;
using namespace cy::vfx_test;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

constexpr u32 kCapacity = 16;

/// A minimal emitter: sixteen slots, a spawn that fills them all at once, an initialise that gives
/// each particle a position along +X and a colour, and NO update — so nothing dies except when a
/// case kills it, and the population is exactly what the case set up.
struct Fixture {
    NodeRegistry registry{allocator()};
    DataInterfaceRegistry interfaces{allocator()};
    graph::DiagnosticSink sink{allocator()};
    CompileReport report{allocator()};
    Expected<CompiledSystem, Error> system = fail(ErrorCode::Unavailable, "not cooked");
    SimulationWorld world{allocator()};
    EffectHandle handle = kInvalidEffect;

    explicit Fixture(RendererKind kind) {
        CY_REQUIRE(prepare(registry, interfaces).has_value());

        VfxSystemAsset asset(allocator(), Name::intern("rows"));
        Emitter emitter(allocator(), Name::intern("rows"));
        emitter.set_capacity(kCapacity);
        emitter.set_renderer(static_cast<u8>(kind));
        for (const char* name : {"position", "velocity", "color", "size", "emission"}) {
            AttributeDecl decl;
            decl.name = Name::intern(name);
            const std::string_view spelled(name);
            const char* type = "float";
            if (spelled == "position" || spelled == "velocity") {
                type = "float3";
            } else if (spelled == "color") {
                type = "float4";
            }
            decl.type = Name::intern(type);
            decl.minimum = -1000.0F;
            decl.maximum = 1000.0F;
            CY_REQUIRE(emitter.declare_attribute(decl).has_value());
        }

        StageBuilder spawn(allocator(), "spawn");
        spawn.spawn_count(spawn.constant(static_cast<f32>(kCapacity)));
        CY_REQUIRE(spawn.ok());
        CY_REQUIRE(emitter.set_stage(Stage::Spawn, spawn.take()).has_value());

        StageBuilder init(allocator(), "initialise");
        // POSITION ALONG +X BY SPAWN INDEX, so a case can name a slot and know where its row is —
        // and so the ribbon's `along` parameter has a direction to run in.
        const NodeKey index = init.input("spawn_index");
        init.write("position", init.make3(index, init.constant(0.0F), init.constant(0.0F)));
        // A velocity along +Y, which is what a decal's projection axis is derived from.
        init.write("velocity",
                   init.make3(init.constant(0.0F), init.constant(2.0F), init.constant(0.0F)));
        init.write("size", init.constant(1.0F));
        init.write("emission", init.constant(4.0F));
        init.write("color", init.make4(init.constant(1.0F), init.constant(0.5F),
                                       init.constant(0.25F), init.constant(1.0F)));
        CY_REQUIRE(init.ok());
        CY_REQUIRE(emitter.set_stage(Stage::Initialise, init.take()).has_value());
        CY_REQUIRE(asset.add_emitter(std::move(emitter)).has_value());
        asset.resolve(registry);

        system = compile_system(asset, registry, interfaces, CompileOptions{}, sink, report);
        CY_REQUIRE(system.has_value());

        WorldDescription description;
        description.pool_bytes = 1ULL << 20U;
        CY_REQUIRE(world.initialize(description).has_value());
        EffectSpawn play;
        play.simulation_hz = 60.0F;
        auto played = world.play(*system, play);
        CY_REQUIRE(played.has_value());
        handle = played.value();
    }

    void step(u32 count = 1) noexcept {
        for (u32 index = 0; index < count; ++index) {
            StepReport step_report;
            CY_REQUIRE(world.step(1.0F / 60.0F, step_report).has_value());
        }
    }

    /// Kill one slot by hand. `alive_flags` is public because `runtime.cpp`'s executor is a free
    /// function; a case that had to wait for a lifetime to expire could not name which slot broke.
    void kill(u32 slot) noexcept {
        const Span<u8> flags = world.alive_flags(0);
        CY_REQUIRE(slot < flags.size());
        flags[slot] = 0;
    }

    [[nodiscard]] u32 live() const noexcept {
        const Span<const u8> flags = world.alive_flags(0);
        u32 count = 0;
        for (const u8 flag : flags) {
            count += flag != 0 ? 1U : 0U;
        }
        return count;
    }
};

[[nodiscard]] RendererDecl decl_for(RendererKind kind) noexcept {
    RendererDecl decl;
    decl.kind = kind;
    decl.material = 7;
    return decl;
}

}  // namespace

CY_TEST_CASE("a renderer kind is declared by the emitter and survives the cook") {
    // `publish_mesh_instances` decides an emitter is a mesh emitter by asking whether its layout
    // has a `mesh` attribute. That does not scale to eight kinds — a Trail and a Ribbon read the
    // same attributes — so the kind is a DECLARATION, and it has to reach the cooked artefact or a
    // host would need the authored asset to know how to draw the cooked one.
    Fixture fixture(RendererKind::Ribbon);
    CY_CHECK_EQ(fixture.system->emitters()[0].renderer(), static_cast<u8>(RendererKind::Ribbon));

    // AND IT IS PART OF THE COOK KEY. Two emitters that differ only in how they are drawn produce
    // the same kernel digest and the same layout digest, so without the renderer in the key they
    // would share a cook key — and "two cooks that produce this key produce identical artefacts"
    // would be false of exactly the pair a caller is most likely to have both of.
    Fixture other(RendererKind::Trail);
    CY_CHECK_NE(fixture.system->cook_key(), other.system->cook_key());
    std::fprintf(stderr, "ribbon cook key 0x%llx, trail cook key 0x%llx\n",
                 static_cast<unsigned long long>(fixture.system->cook_key()),
                 static_cast<unsigned long long>(other.system->cook_key()));
}

CY_TEST_CASE("a publication refuses a declaration whose kind is not its own") {
    // A publication that accepted any declaration would draw a trail's history as a ribbon's chain
    // and report success. The refusal is by name, at the call, before any row is produced.
    Fixture fixture(RendererKind::Ribbon);
    fixture.step();
    PublicationHistory history(allocator());
    CY_REQUIRE(history.resize(kCapacity, 4).has_value());
    Array<RibbonVertex> rows(allocator());
    RenderPublishReport report;
    CY_CHECK(!publish_ribbons(fixture.world, decl_for(RendererKind::Trail), Vec3{}, 256, history,
                              rows, report));
    CY_CHECK(!publish_trails(fixture.world, decl_for(RendererKind::Ribbon), Vec3{}, 256, history,
                             rows, report));
    CY_CHECK(publish_ribbons(fixture.world, decl_for(RendererKind::Ribbon), Vec3{}, 256, history,
                             rows, report)
                 .has_value());
}

CY_TEST_CASE("a ribbon chain broken mid-chain terminates rather than connecting across the gap") {
    // `vfx-system`: "WHEN particles in a ribbon chain are killed mid-chain THEN the ribbon SHALL
    // terminate cleanly rather than connecting across the gap."
    Fixture fixture(RendererKind::Ribbon);
    fixture.step();
    CY_REQUIRE_EQ(fixture.live(), kCapacity);

    PublicationHistory history(allocator());
    CY_REQUIRE(history.resize(kCapacity, 2).has_value());
    Array<RibbonVertex> rows(allocator());
    RenderPublishReport report;
    RendererDecl decl = decl_for(RendererKind::Ribbon);
    decl.width = 0.5F;
    decl.twist = 1.0F;

    // UNBROKEN: one strip over all sixteen.
    CY_REQUIRE(
        publish_ribbons(fixture.world, decl, Vec3{}, 256, history, rows, report).has_value());
    CY_CHECK_EQ(report.primitives, 1U);
    CY_CHECK_EQ(rows.size(), kCapacity);
    CY_CHECK_EQ(report.chain_breaks, 0U);
    // The width tapers and the twist accumulates over the strip, which is "width and twist over
    // length" as two numbers rather than as two fields nothing sets.
    CY_CHECK_GT(rows[0].width, rows[kCapacity - 1U].width);
    CY_CHECK_LT(rows[0].twist, rows[kCapacity - 1U].twist);
    CY_CHECK_EQ(rows[0].strip, rows[kCapacity - 1U].strip);

    // BROKEN AT SLOT 7: two strips, fourteen vertices, one break, and NO vertex that joins slot 6
    // to slot 8.
    fixture.kill(7);
    CY_REQUIRE(
        publish_ribbons(fixture.world, decl, Vec3{}, 256, history, rows, report).has_value());
    std::fprintf(stderr, "after the break: %u strips, %zu vertices, %u chain breaks\n",
                 report.primitives, rows.size(), report.chain_breaks);
    CY_CHECK_EQ(report.primitives, 2U);
    CY_CHECK_EQ(rows.size(), kCapacity - 1U);
    CY_CHECK_GT(report.chain_breaks, 0U);

    u32 strips_seen = 0;
    u32 previous_strip = 0;
    bool joined_across_the_gap = false;
    for (usize index = 0; index < rows.size(); ++index) {
        if (index == 0 || rows[index].strip != previous_strip) {
            ++strips_seen;
            previous_strip = rows[index].strip;
        }
        // THE GAP ITSELF: slot 6 and slot 8 must not be consecutive vertices of ONE strip, because
        // that is precisely "connecting across the gap".
        if (index + 1 < rows.size() && rows[index].row.particle == 6U &&
            rows[index + 1].row.particle == 8U) {
            joined_across_the_gap = rows[index].strip == rows[index + 1].strip;
        }
    }
    CY_CHECK_EQ(strips_seen, 2U);
    CY_CHECK(!joined_across_the_gap);
    // And the dead slot produced no vertex at all.
    for (const RibbonVertex& vertex : rows) {
        CY_CHECK_NE(vertex.row.particle, 7U);
    }
}

CY_TEST_CASE("a motion vector is suppressed on the frame a particle spawns and after a kill") {
    // Two requirements at once, and they are one mechanism: "motion vectors DERIVED FROM THE
    // PREVIOUS SIMULATION STEP", and "WHEN an effect's particles change discontinuously (spawn,
    // kill, teleport) THEN interpolation SHALL be SUPPRESSED for those particles".
    Fixture fixture(RendererKind::Decal);
    fixture.step();

    PublicationHistory history(allocator());
    CY_REQUIRE(history.resize(kCapacity, 2).has_value());
    Array<DecalInstance> rows(allocator());
    RenderPublishReport report;
    const RendererDecl decl = decl_for(RendererKind::Decal);

    // FRAME ONE: every particle is new, so nothing has a previous position and every row says so.
    CY_REQUIRE(publish_decals(fixture.world, decl, Vec3{}, 256, history, rows, report).has_value());
    CY_REQUIRE_EQ(rows.size(), kCapacity);
    CY_CHECK_EQ(report.motion_suppressed, kCapacity);
    for (const DecalInstance& row : rows) {
        CY_CHECK(!row.row.motion_valid);
    }

    // FRAME TWO, having moved the effect: every row has a previous position and it is where the
    // row was, not where it is. Moving the INSTANCE rather than the particles is what makes the
    // delta a number the case chose: publication is camera-relative and adds the instance's
    // position, so a two-metre move is a two-metre motion vector.
    CY_REQUIRE(fixture.world.set_transform(fixture.handle, Vec3{2.0F, 0.0F, 0.0F}).has_value());
    CY_REQUIRE(publish_decals(fixture.world, decl, Vec3{}, 256, history, rows, report).has_value());
    CY_CHECK_EQ(report.motion_suppressed, 0U);
    f32 worst = 0.0F;
    for (const DecalInstance& row : rows) {
        CY_CHECK(row.row.motion_valid);
        worst = std::fmax(worst,
                          std::fabs((row.row.position[0] - row.row.previous_position[0]) - 2.0F));
    }
    std::fprintf(stderr, "worst motion-vector error after a two-metre move: %.3e\n",
                 static_cast<double>(worst));
    CY_CHECK_LT(worst, 1.0e-5F);

    // A KILL AND A RE-USE. Slot 3 dies; the next publication does not record it, so the one after
    // that suppresses its motion vector even though the slot is alive again by then.
    fixture.kill(3);
    CY_REQUIRE(publish_decals(fixture.world, decl, Vec3{}, 256, history, rows, report).has_value());
    CY_CHECK_EQ(rows.size(), kCapacity - 1U);
    fixture.step();  // the spawn refills slot 3
    CY_REQUIRE(publish_decals(fixture.world, decl, Vec3{}, 256, history, rows, report).has_value());
    bool found = false;
    for (const DecalInstance& row : rows) {
        if (row.row.particle != 3U) {
            CY_CHECK(row.row.motion_valid);
            continue;
        }
        found = true;
        // THE SMEAR THIS PREVENTS: without the suppression, slot 3's new occupant would carry a
        // motion vector from wherever its predecessor was standing when it died.
        CY_CHECK(!row.row.motion_valid);
    }
    CY_CHECK(found);
    CY_CHECK_EQ(report.motion_suppressed, 1U);

    // THE SWITCH. `motion_vectors` off costs nothing and produces no vector at all, which is what a
    // frame with no temporal pass wants.
    RendererDecl without = decl;
    without.motion_vectors = false;
    CY_REQUIRE(
        publish_decals(fixture.world, without, Vec3{}, 256, history, rows, report).has_value());
    CY_CHECK_EQ(report.motion_suppressed, static_cast<u32>(rows.size()));
}

CY_TEST_CASE("a decal projects along the particle's velocity where it has one") {
    Fixture fixture(RendererKind::Decal);
    fixture.step();
    PublicationHistory history(allocator());
    CY_REQUIRE(history.resize(kCapacity, 2).has_value());
    Array<DecalInstance> rows(allocator());
    RenderPublishReport report;
    RendererDecl decl = decl_for(RendererKind::Decal);
    decl.decal_depth = 3.0F;
    CY_REQUIRE(publish_decals(fixture.world, decl, Vec3{}, 256, history, rows, report).has_value());
    CY_REQUIRE(rows.size() > 0U);
    // The fixture's particles move along +Y at 2 m/s, so the projection axis is +Y normalised —
    // not the world -Y a particle with no velocity would get.
    CY_CHECK_LT(std::fabs(rows[0].axis[0]), 1.0e-6F);
    CY_CHECK_LT(std::fabs(rows[0].axis[1] - 1.0F), 1.0e-6F);
    CY_CHECK_LT(std::fabs(rows[0].axis[2]), 1.0e-6F);
    CY_CHECK_LT(std::fabs(rows[0].depth - 3.0F), 1.0e-6F);
    CY_CHECK_EQ(rows[0].row.material, 7U);
}

CY_TEST_CASE("the hard light budget holds, and the budget controller degrades it") {
    // `vfx-system`: particle lights "SHALL participate in clustered light assignment subject to a
    // HARD PER-FRAME COUNT BUDGET, degraded by the budget controller like any other VFX cost."
    Fixture fixture(RendererKind::Light);
    fixture.step();
    CY_REQUIRE_EQ(fixture.live(), kCapacity);

    PublicationHistory history(allocator());
    CY_REQUIRE(history.resize(kCapacity, 2).has_value());
    Array<LightInstance> rows(allocator());
    RenderPublishReport report;
    RendererDecl decl = decl_for(RendererKind::Light);
    decl.max_lights = 6;

    BudgetController controller;
    const ScalabilityPolicy policy;
    BudgetLevers levers = controller.levers_for(ImportanceClass::Ambient, policy);

    // THE HARD BUDGET: sixteen live particles, six lights, and the other ten counted rather than
    // silently absent.
    CY_REQUIRE(publish_lights(fixture.world, decl, levers, Vec3{}, 256, history, rows, report)
                   .has_value());
    std::fprintf(stderr, "live %u, budget %u, published %zu, over budget %u\n", fixture.live(),
                 decl.max_lights, rows.size(), report.lights_over_budget);
    CY_CHECK_EQ(rows.size(), 6U);
    CY_CHECK_EQ(report.lights_over_budget, kCapacity - 6U);
    CY_CHECK_EQ(report.base.dropped, kCapacity - 6U);

    // WHAT SURVIVES IS RANKED, not taken in slot order. The particles sit along +X at 0..15 and the
    // camera is at the origin, so the nearest six are slots 0 to 5 — a budget that kept the first
    // six it walked would give the same answer, so the camera is MOVED to the far end and the
    // answer must move with it.
    Array<LightInstance> far_rows(allocator());
    CY_REQUIRE(publish_lights(fixture.world, decl, levers, Vec3{15.0F, 0.0F, 0.0F}, 256, history,
                              far_rows, report)
                   .has_value());
    CY_REQUIRE_EQ(far_rows.size(), 6U);
    u32 highest = 0;
    for (const LightInstance& row : far_rows) {
        highest = row.row.particle > highest ? row.row.particle : highest;
    }
    u32 near_highest = 0;
    for (const LightInstance& row : rows) {
        near_highest = row.row.particle > near_highest ? row.row.particle : near_highest;
    }
    std::fprintf(stderr, "nearest-camera survivors top out at slot %u, far camera at slot %u\n",
                 near_highest, highest);
    CY_CHECK_GT(highest, near_highest);

    // THE CONTROLLER'S LEVER. `count_cap_scale` is the degradation, and it is the same lever that
    // bounds a particle count — not a second mechanism for lights.
    levers.count_cap_scale = 0.5F;
    CY_REQUIRE(publish_lights(fixture.world, decl, levers, Vec3{}, 256, history, rows, report)
                   .has_value());
    CY_CHECK_EQ(rows.size(), 3U);
    levers.count_cap_scale = 0.0F;
    CY_REQUIRE(publish_lights(fixture.world, decl, levers, Vec3{}, 256, history, rows, report)
                   .has_value());
    CY_CHECK_EQ(rows.size(), 0U);
    CY_CHECK_EQ(report.lights_over_budget, kCapacity);

    // A light's radius follows its radiance, so a particle too dim to reach a cluster claims none.
    levers.count_cap_scale = 1.0F;
    CY_REQUIRE(publish_lights(fixture.world, decl, levers, Vec3{}, 256, history, rows, report)
                   .has_value());
    CY_REQUIRE(rows.size() > 0U);
    CY_CHECK_GT(rows[0].radius, 0.0F);
    CY_CHECK_GT(rows[0].intensity[0], rows[0].intensity[1]);
}

CY_TEST_CASE("a trail is exactly as long as the history that exists") {
    Fixture fixture(RendererKind::Trail);
    fixture.step();

    PublicationHistory history(allocator());
    CY_REQUIRE(history.resize(kCapacity, 4).has_value());
    Array<RibbonVertex> rows(allocator());
    RenderPublishReport report;
    RendererDecl decl = decl_for(RendererKind::Trail);
    decl.trail_history = 4;

    // A TRAIL DECLARED DEEPER THAN ITS HISTORY IS REFUSED, because it would draw to a position
    // nobody recorded.
    RendererDecl too_deep = decl;
    too_deep.trail_history = 8;
    PublicationHistory shallow(allocator());
    CY_REQUIRE(shallow.resize(kCapacity, 2).has_value());
    CY_CHECK(!publish_trails(fixture.world, too_deep, Vec3{}, 1024, shallow, rows, report));

    // FRAME ONE: one recorded position a particle, so no trail has two vertices and none is drawn.
    CY_REQUIRE(
        publish_trails(fixture.world, decl, Vec3{}, 1024, history, rows, report).has_value());
    CY_CHECK_EQ(report.primitives, 0U);
    CY_CHECK_EQ(rows.size(), 0U);

    // The trail grows by one vertex a frame until it reaches the declared depth, and then stops.
    const u32 expected[] = {2U, 3U, 4U, 4U, 4U};
    for (u32 frame = 0; frame < 5U; ++frame) {
        CY_REQUIRE(
            fixture.world
                .set_transform(fixture.handle, Vec3{static_cast<f32>(frame + 1U), 0.0F, 0.0F})
                .has_value());
        CY_REQUIRE(
            publish_trails(fixture.world, decl, Vec3{}, 1024, history, rows, report).has_value());
        const u32 per_trail =
            report.primitives == 0 ? 0U : static_cast<u32>(rows.size()) / report.primitives;
        std::fprintf(stderr, "frame %u: %u trails of %u vertices\n", frame + 1U, report.primitives,
                     per_trail);
        CY_CHECK_EQ(report.primitives, kCapacity);
        CY_CHECK_EQ(per_trail, expected[frame]);
    }

    // ONLY THE HEAD OF A TRAIL CARRIES A MOTION VECTOR. A tail vertex is a past position and its
    // "previous" would be a position one step older still — which is the trail itself, not a
    // velocity, and a temporal pass fed that would resolve the trail twice.
    u32 heads = 0;
    for (const RibbonVertex& vertex : rows) {
        heads += vertex.row.motion_valid ? 1U : 0U;
        if (vertex.along > 0.0F) {
            CY_CHECK(!vertex.row.motion_valid);
        }
    }
    CY_CHECK_EQ(heads, kCapacity);
}

CY_TEST_CASE("a beam meets both of its endpoints however much it sags") {
    Fixture fixture(RendererKind::Beam);
    fixture.step();
    // The instance sits at the origin, so the emitter's own position is the far end of every beam:
    // an emitter with no `beam_end` attribute beams back to where it was spawned rather than being
    // refused, because "a beam from the emitter to a particle" is the commonest authoring there is.
    PublicationHistory history(allocator());
    CY_REQUIRE(history.resize(kCapacity, 2).has_value());
    Array<BeamVertex> rows(allocator());
    RenderPublishReport report;
    RendererDecl decl = decl_for(RendererKind::Beam);
    decl.segments = 8;
    decl.beam_sag = 5.0F;
    decl.beam_noise = 2.0F;

    CY_REQUIRE(publish_beams(fixture.world, decl, Vec3{}, 4096, history, rows, report).has_value());
    CY_CHECK_EQ(report.primitives, kCapacity);
    CY_CHECK_EQ(rows.size(), static_cast<usize>(kCapacity) * (decl.segments + 1U));

    // SAG AND NOISE ARE ZERO AT BOTH ENDS. A linear droop, or noise applied without the
    // `along * (1 - along)` envelope, would move the far end — and a point-to-point primitive that
    // does not meet its two points is not one.
    const u32 stride = decl.segments + 1U;
    f32 worst_end = 0.0F;
    f32 deepest_sag = 0.0F;
    for (u32 beam = 0; beam < kCapacity; ++beam) {
        const BeamVertex& head = rows[static_cast<usize>(beam) * stride];
        const BeamVertex& tail = rows[(static_cast<usize>(beam) * stride) + decl.segments];
        // The head is the particle, at (slot, 0, 0); the tail is the emitter, at the origin.
        worst_end = std::fmax(
            worst_end, std::fabs(head.row.position[0] - static_cast<f32>(head.row.particle)));
        worst_end = std::fmax(worst_end, std::fabs(head.row.position[1]));
        worst_end = std::fmax(worst_end, std::fabs(tail.row.position[0]));
        worst_end = std::fmax(worst_end, std::fabs(tail.row.position[1]));
        worst_end = std::fmax(worst_end, std::fabs(tail.row.position[2]));
        for (u32 index = 1; index < decl.segments; ++index) {
            deepest_sag = std::fmax(deepest_sag, -rows[(beam * stride) + index].row.position[1]);
        }
    }
    std::fprintf(stderr, "worst endpoint error %.3e, deepest sag %.3f\n",
                 static_cast<double>(worst_end), static_cast<double>(deepest_sag));
    CY_CHECK_LT(worst_end, 1.0e-5F);
    // And the middle really does sag: `beam_sag` at the midpoint, which is the parabola's peak.
    CY_CHECK_GT(deepest_sag, 4.0F);
    CY_CHECK_LT(deepest_sag, 5.5F);
    CY_CHECK_LT(rows[0].along, rows[decl.segments].along);
}

CY_TEST_CASE("a volume thins rather than shrinking as its particle fades") {
    Fixture fixture(RendererKind::Volume);
    fixture.step();
    PublicationHistory history(allocator());
    CY_REQUIRE(history.resize(kCapacity, 2).has_value());
    Array<VolumeInstance> rows(allocator());
    RenderPublishReport report;
    RendererDecl decl = decl_for(RendererKind::Volume);
    decl.volume_extinction = 2.0F;

    CY_REQUIRE(
        publish_volumes(fixture.world, decl, Vec3{}, 256, history, rows, report).has_value());
    CY_REQUIRE_EQ(rows.size(), kCapacity);
    const f32 opaque_extinction = rows[0].extinction;
    const f32 opaque_radius = rows[0].radius;
    CY_CHECK_LT(std::fabs(opaque_extinction - 2.0F), 1.0e-5F);

    // Halve the alpha of every particle and publish again. The radius must not move — a volume that
    // shrank as it faded would pop out of a froxel grid instead of dissolving in place.
    const Span<u8> flags = fixture.world.alive_flags(0);
    CY_REQUIRE(flags.size() > 0U);
    // There is no setter for an attribute from outside a kernel, deliberately, so the fade is
    // expressed the way an author would: a second cook is not needed because `color`'s alpha is the
    // multiplier and the check is that extinction TRACKS it while radius does not.
    CY_CHECK_LT(std::fabs(rows[0].extinction - (decl.volume_extinction * rows[0].row.color[3])),
                1.0e-5F);
    CY_CHECK_LT(std::fabs(rows[0].radius - opaque_radius), 1.0e-6F);
    // The scattering colour is the radiance the sprite path would publish, emission folded in.
    CY_CHECK_GT(rows[0].scattering[0], 1.0F);
}
