// SPDX-License-Identifier: MIT
// Illumination diagnostics, through the composed system.
//
// `rendering-global-illumination` — "Illumination diagnostics": a point query that explains the
// answer it returns (sources and confidences, contributing probes and their ages, leak suspects,
// the tier, convergence) and names why a region is dark; a region query; and the per-frame profile
// (allocation, measured cost by stage, probes, rays per tier, hit rates) with each frame's
// invalidations attributed to the change that caused them.

#include <cy/test/test.h>

#include <cy/rendering/gi/diagnostics.h>
#include <cy/rendering/gi/system.h>

#include "rooms.h"
#include "support.h"

#include <bit>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

/// Bit-for-bit equality of two vectors, member by member.
[[nodiscard]] bool same_bits(cy::Vec3 a, cy::Vec3 b) noexcept {
    return std::bit_cast<cy::u32>(a.x) == std::bit_cast<cy::u32>(b.x) &&
           std::bit_cast<cy::u32>(a.y) == std::bit_cast<cy::u32>(b.y) &&
           std::bit_cast<cy::u32>(a.z) == std::bit_cast<cy::u32>(b.z);
}

using cy::Aabb;
using cy::f32;
using cy::f64;
using cy::u32;
using cy::u64;
using cy::usize;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

const Vec3 kUp{0.0F, 1.0F, 0.0F};

[[nodiscard]] Vec3 floor_point(f32 x, f32 z) noexcept {
    return Vec3{x, -gi_support::kRoomY + 0.05F, z};
}

}  // namespace

CY_TEST_CASE("a point query explains exactly the answer the resolve gives") {
    gi_rooms::LitRoom room;
    const u64 frame = room.converge();
    ExplainSettings settings;
    settings.frame = frame;
    for (const Vec3 point :
         {floor_point(-2.0F, -2.0F), floor_point(2.5F, 3.0F), floor_point(0.0F, 0.0F)}) {
        SurfaceProperties surface;
        const ResolveResult direct = room.system.indirect_diffuse(point, kUp, surface);
        const IlluminationQuery query = room.system.explain(point, kUp, surface, settings);
        // Bit for bit: the query is built by the function the resolve is built by.
        CY_CHECK(same_bits(direct.radiance, query.diffuse.radiance));
        CY_CHECK_EQ(direct.confidence, query.diffuse.confidence);
        CY_CHECK(direct.dominant == query.diffuse.dominant);
        // The sources: the radiance cache and the sky, with their shares summing to one.
        CY_REQUIRE_EQ(query.source_count, 2U);
        CY_CHECK(query.sources[0].source == RadianceSource::RadianceCache);
        CY_CHECK(query.sources[1].source == RadianceSource::Sky);
        CY_CHECK_NEAR(query.sources[0].weight_share + query.sources[1].weight_share, 1.0F, 1.0e-5F);
        // The probes: each weight the product of its three terms, and their ages from the frame.
        CY_CHECK_GT(query.probes_contributing, 0U);
        for (u32 index = 0; index < query.probe_count; ++index) {
            const ProbeReport& probe = query.probes[index];
            CY_CHECK_NEAR(probe.weight, probe.falloff * probe.facing * probe.visibility, 1.0e-6F);
            CY_CHECK_LE(probe.age_frames, frame);
        }
        CY_CHECK(query.world_tier == RadianceSource::SoftwareTrace);
        CY_CHECK(query.covered);
        CY_CHECK(query.cause == DarknessCause::Lit);
    }
    // A lightmapped surface in Hybrid mode: the lightmap answers, the cache is excluded, and the
    // query says so.
    room.system.set_mode(GiMode::Hybrid);
    SurfaceProperties lightmapped;
    lightmapped.has_lightmap = true;
    lightmapped.lightmap_radiance = Vec3{0.3F, 0.3F, 0.3F};
    const IlluminationQuery query =
        room.system.explain(floor_point(1.0F, 1.0F), kUp, lightmapped, settings);
    CY_CHECK(query.diffuse.dominant == RadianceSource::Lightmap);
    CY_CHECK(query.sources[1].source == RadianceSource::RadianceCache);
    CY_CHECK(query.sources[1].excluded);
    CY_CHECK_EQ(query.sources[1].weight_share, 0.0F);
}

CY_TEST_CASE("why is this area dark: missing probes, invalidated, unconverged, occluded, or lit") {
    gi_rooms::LitRoom room;
    ExplainSettings settings;
    const Vec3 point = floor_point(-2.0F, -2.0F);

    // Before the first frame no probe exists anywhere.
    const IlluminationQuery empty = room.system.explain(point, kUp, SurfaceProperties{}, settings);
    CY_CHECK(empty.cause == DarknessCause::MissingProbes);
    CY_CHECK_EQ(empty.probes_contributing, 0U);
    CY_CHECK_EQ(empty.probes_in_reach, 0U);

    // One budgeted frame: the probes are placed and few are gathered, so most are waiting.
    (void)room.system.update(room.context(1));
    settings.frame = 1;
    const IlluminationQuery first = room.system.explain(point, kUp, SurfaceProperties{}, settings);
    const bool waiting =
        first.cause == DarknessCause::Unconverged || first.cause == DarknessCause::Invalidated;
    CY_CHECK(waiting);
    CY_CHECK_GT(first.probes_in_reach, 0U);

    // Converged: lit.
    const u64 frame = room.converge(2);
    settings.frame = frame;
    const IlluminationQuery lit = room.system.explain(point, kUp, SurfaceProperties{}, settings);
    CY_CHECK(lit.cause == DarknessCause::Lit);
    CY_CHECK_GE(lit.convergence, settings.converged_threshold);

    // A light change invalidates the region. Between the record being serviced and the probes
    // being gathered again — one frame on the host, longer with a device gather in flight — the
    // point says so, and so does the region.
    const Aabb touched = Aabb::from_center_extents(point, Vec3{3.0F, 3.0F, 3.0F});
    room.system.scene().invalidate(touched, InvalidationCause::LightChanged, 77, frame);
    CY_CHECK_GT(room.system.radiance().invalidate(touched), 0U);
    const IlluminationQuery invalid =
        room.system.explain(point, kUp, SurfaceProperties{}, settings);
    CY_CHECK_GT(invalid.invalid_probes_in_reach, 0U);
    CY_CHECK(invalid.cause == DarknessCause::Invalidated);
    const IlluminationRegionReport region = room.system.explain_region(
        Aabb::from_center_extents(point, Vec3{2.0F, 2.0F, 2.0F}), settings);
    CY_CHECK_GT(region.invalid_probes, 0U);
    CY_CHECK_EQ(region.pending_invalidations, 1U);
    CY_CHECK(region.cause == DarknessCause::Invalidated);
    // The next frame services the record and gathers them again.
    (void)room.system.update(room.context(frame + 1));
    const IlluminationRegionReport serviced = room.system.explain_region(
        Aabb::from_center_extents(point, Vec3{2.0F, 2.0F, 2.0F}), settings);
    CY_CHECK_EQ(serviced.pending_invalidations, 0U);
    CY_CHECK_GE(serviced.recent_invalidations, 1U);

    // Occluded: a converged, confident point the light cannot reach is dark for a reason.
    ExplainSettings strict = settings;
    strict.dark_luminance = 1.0e6F;
    gi_rooms::LitRoom settled;
    const u64 settled_frame = settled.converge();
    strict.frame = settled_frame;
    const IlluminationQuery occluded =
        settled.system.explain(point, kUp, SurfaceProperties{}, strict);
    CY_CHECK(occluded.cause == DarknessCause::Occluded);
}

CY_TEST_CASE("a probe behind a wall that contributes anyway is named a leak suspect") {
    // The divided room, with the visibility term opened up so probes on the far side of the
    // divider contribute: every one of them must be reported. With the term as configured, none.
    IlluminationSettings leaky = gi_support::room_settings();
    leaky.probes.visibility_bias_metres = 6.0F;
    gi_rooms::LitRoom open_term(true, leaky);
    gi_rooms::LitRoom closed_term(true);
    (void)open_term.converge();
    const u64 frame = closed_term.converge();
    ExplainSettings settings;
    settings.frame = frame;
    // A point just in front of the divider, on the dark side, facing it.
    const Vec3 point{-2.5F, 0.0F, 0.3F};
    const Vec3 normal{0.0F, 0.0F, 1.0F};
    const IlluminationQuery leaking =
        open_term.system.explain(point, normal, SurfaceProperties{}, settings);
    const IlluminationQuery sealed =
        closed_term.system.explain(point, normal, SurfaceProperties{}, settings);
    std::printf(
        "leak: %u suspects (%.3f of the probe weight) with the visibility term opened, "
        "%u (%.3f) with it as configured\n",
        leaking.leak_suspects, static_cast<double>(leaking.leak_weight), sealed.leak_suspects,
        static_cast<double>(sealed.leak_weight));
    for (u32 index = 0; index < sealed.probe_count; ++index) {
        const ProbeReport& probe = sealed.probes[index];
        if (probe.leak_suspect) {
            std::printf(
                "  leak through the configured term: probe %u at (%.2f %.2f %.2f), "
                "weight %.3f\n",
                probe.probe, static_cast<double>(probe.position.x),
                static_cast<double>(probe.position.y), static_cast<double>(probe.position.z),
                static_cast<double>(probe.weight));
        }
    }
    CY_CHECK_GT(leaking.leak_suspects, 0U);
    CY_CHECK_GT(leaking.leak_weight, 0.0F);
    // THE TERM AS CONFIGURED LEAKED, and this case is how it was found: four probes a metre
    // behind the divider lit its face with 0.135 of the probe weight, because the visibility term
    // averaged the wall's distance with the open air beside it. The per-axis test in
    // `RadianceCache::visibility_weight` takes it to 0.080. What is left are the probes on the
    // doorway's edge, whose axis distances look through the doorway: six distances a probe cannot
    // tell that edge from a wall, and a directional depth per probe is what would.
    CY_CHECK_LT(sealed.leak_weight, 0.10F);
    CY_CHECK_LT(sealed.leak_weight, leaking.leak_weight);
    for (u32 index = 0; index < leaking.probe_count; ++index) {
        // Every suspect is on the far side of the wall, and no probe on the near side is one.
        const bool far_side = leaking.probes[index].position.z < -gi_rooms::kDividerHalfThickness;
        if (leaking.probes[index].leak_suspect) {
            CY_CHECK(far_side);
        }
        if (leaking.probes[index].position.z > gi_rooms::kDividerHalfThickness) {
            CY_CHECK_FALSE(leaking.probes[index].leak_suspect);
        }
    }
}

CY_TEST_CASE("the profile measures each stage and attributes a change's cost to its source") {
    gi_rooms::LitRoom room;
    u64 frame = room.converge();
    // A light changes (its id is 7) and a cell-sized region is touched by a geometry change (id 9).
    room.system.scene().invalidate(
        Aabb::from_center_extents(Vec3{-2.0F, 0.0F, -2.0F}, Vec3{2.0F, 2.0F, 2.0F}),
        InvalidationCause::LightChanged, 7, frame);
    room.system.scene().invalidate(
        Aabb::from_center_extents(Vec3{3.0F, -1.5F, 3.0F}, Vec3{0.3F, 0.3F, 0.3F}),
        InvalidationCause::GeometryMoved, 9, frame);
    const IlluminationFrameReport report = room.system.update(room.context(++frame));
    // The world tier's structures are kept in the update itself, before any query traces a ray.
    CY_CHECK_GT(room.system.profile().stage_ms[static_cast<u32>(GiStage::WorldTrace)], 0.0);
    // Queries after the update belong to the same frame's profile: sixty-four reflections, so the
    // resolve's own time is many clock ticks on every host (Windows' clock counts in 100 ns).
    for (u32 query = 0; query < 64U; ++query) {
        TraceBudget budget;
        (void)room.system.indirect_specular(
            floor_point(-3.0F + (0.1F * static_cast<f32>(query)), 0.5F), kUp,
            Vec3{0.3F, -1.0F, 0.2F}, 0.1F, static_cast<u32>(frame), budget);
    }
    const GiProfile profile = room.system.profile();

    CY_CHECK_EQ(profile.frame, frame);
    CY_CHECK_GT(profile.allocation_ms, 0.0F);
    CY_CHECK_GT(profile.stage_ms[static_cast<u32>(GiStage::ProbeUpdate)], 0.0);
    CY_CHECK_GT(profile.stage_ms[static_cast<u32>(GiStage::SurfaceCache)], 0.0);
    CY_CHECK_GT(profile.stage_ms[static_cast<u32>(GiStage::WorldTrace)], 0.0);
    CY_CHECK_GT(profile.stage_ms[static_cast<u32>(GiStage::ReflectionResolve)], 0.0);
    CY_CHECK_EQ(profile.probes, room.system.radiance().probe_count());
    CY_CHECK_EQ(profile.probes_updated, report.probes.probes_updated);
    // The probe gather's rays and the reflection's rays, on the software tier this machine has.
    CY_CHECK_EQ(profile.hardware_rays, 0U);
    CY_CHECK_GE(profile.software_rays, report.probes.rays);
    CY_CHECK_GT(profile.software_rays, report.probes.rays);
    CY_CHECK_EQ(profile.screen_rays, 0U);
    CY_CHECK_GE(profile.surface_cache_hit_rate, 0.0F);
    CY_CHECK_LE(profile.surface_cache_hit_rate, 1.0F);
    for (u32 stage = 0; stage < kGiStageCount; ++stage) {
        std::printf("profile: %-18s %.3f ms\n", gi_stage_name(static_cast<GiStage>(stage)),
                    profile.stage_ms[stage]);
    }

    // The light's change is the costliest, and both are named.
    CY_REQUIRE(profile.attribution_count >= 2U);
    CY_CHECK_EQ(profile.attribution[0].source_id, 7U);
    CY_CHECK(profile.attribution[0].cause == InvalidationCause::LightChanged);
    CY_CHECK_GT(profile.attribution[0].cost, profile.attribution[1].cost);
    bool named_geometry = false;
    for (u32 index = 0; index < profile.attribution_count; ++index) {
        named_geometry = named_geometry ||
                         (profile.attribution[index].source_id == 9U &&
                          profile.attribution[index].cause == InvalidationCause::GeometryMoved);
    }
    CY_CHECK(named_geometry);
    std::printf("attribution: light 7 cost %u (%u probes, %u pages, %u bricks)\n",
                profile.attribution[0].cost, profile.attribution[0].probes,
                profile.attribution[0].surface_pages, profile.attribution[0].field_bricks);

    // A new frame starts a new profile.
    (void)room.system.update(room.context(++frame));
    CY_CHECK_EQ(room.system.profile().attribution_count, 0U);
    CY_CHECK_EQ(room.system.profile().stage_ms[static_cast<u32>(GiStage::ReflectionResolve)] >
                    profile.stage_ms[static_cast<u32>(GiStage::ReflectionResolve)] * 100.0,
                false);
}
