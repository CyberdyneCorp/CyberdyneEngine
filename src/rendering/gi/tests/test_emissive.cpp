// SPDX-License-Identifier: MIT
// Emissive surfaces as illumination: the form factor a promoted emitter is integrated with, the
// classification by radiant power and area, and both paths — the surface cache and promotion —
// measured against the offline path tracer over the same room.
//
// `rendering-global-illumination` — "Emissive surfaces as illumination". The CPU half here is the
// oracle `cy::rendering-gi-gpu`'s card lighting transcribes; `render.gi_gpu` holds the device to
// it.
//
// THE ROOM. A closed 4 x 2 x 4 m box of grey cards half a metre apart, with no light placed and a
// black sky: whatever lights it is an emitter. Two representations of the one room, as everywhere
// in this module — `BoxProxyScene` slabs that the path tracer traces exactly, and the distance
// field the real-time card gather marches — and one set of cards both resolve their surfaces
// through.

#include <cy/test/test.h>

#include <cy/core/math/matrix.h>
#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi/card_lighting.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/emissive.h>
#include <cy/rendering/gi/proxy_scene.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/surface_cache.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi/tracing.h>

#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <vector>

namespace {

using cy::Aabb;
using cy::Array;
using cy::f32;
using cy::f64;
using cy::Span;
using cy::u32;
using cy::u64;
using cy::usize;
using cy::Vec3;
using cy::rendering::gi::BakeSettings;
using cy::rendering::gi::BoxProxyScene;
using cy::rendering::gi::CardGather;
using cy::rendering::gi::CardGatherSettings;
using cy::rendering::gi::CardSnapshot;
using cy::rendering::gi::ClipmapSettings;
using cy::rendering::gi::DistanceField;
using cy::rendering::gi::EmissiveReport;
using cy::rendering::gi::EmissiveSettings;
using cy::rendering::gi::EmitterClass;
using cy::rendering::gi::EmitterRecord;
using cy::rendering::gi::GiLight;
using cy::rendering::gi::GiScene;
using cy::rendering::gi::PathTracer;
using cy::rendering::gi::ProxyBox;
using cy::rendering::gi::SkyTerm;
using cy::rendering::gi::SoftwareTracer;
using cy::rendering::gi::SurfaceCache;
using cy::rendering::gi::SurfacePage;
using cy::rendering::gi::SurfaceUpdateContext;
using cy::rendering::gi::Surfel;

constexpr Vec3 kHalf{2.0F, 1.0F, 2.0F};
constexpr f32 kSpacing = 0.5F;
constexpr Vec3 kGrey{0.6F, 0.6F, 0.6F};
constexpr u32 kPanelInstance = 9;
constexpr u32 kLampInstance = 10;

SkyTerm dark_sky() noexcept {
    SkyTerm sky;
    sky.intensity = 0.0F;
    return sky;
}

Surfel card(Vec3 position, Vec3 normal, u32 instance) noexcept {
    Surfel surfel;
    surfel.position = position;
    surfel.normal = normal;
    surfel.albedo = kGrey;
    surfel.roughness = 0.9F;
    surfel.area = kSpacing * kSpacing;
    surfel.instance_id = instance;
    surfel.material_id = instance;
    return surfel;
}

/// Every card of the room, inward facing. Integer loops, for support.h's reason.
std::vector<Surfel> room_cards() {
    std::vector<Surfel> cards;
    const auto steps = [](f32 half) {
        return static_cast<u32>(std::lround(2.0F * half / kSpacing)) + 1U;
    };
    const auto at = [](u32 index, f32 half) {
        return -half + (static_cast<f32>(index) * kSpacing);
    };
    for (u32 ix = 0; ix < steps(kHalf.x); ++ix) {
        for (u32 iz = 0; iz < steps(kHalf.z); ++iz) {
            const f32 x = at(ix, kHalf.x);
            const f32 z = at(iz, kHalf.z);
            cards.push_back(card(Vec3{x, -kHalf.y, z}, Vec3{0.0F, 1.0F, 0.0F}, 1));
            cards.push_back(card(Vec3{x, kHalf.y, z}, Vec3{0.0F, -1.0F, 0.0F}, 2));
        }
    }
    for (u32 iy = 0; iy < steps(kHalf.y); ++iy) {
        const f32 y = at(iy, kHalf.y);
        for (u32 iz = 0; iz < steps(kHalf.z); ++iz) {
            const f32 z = at(iz, kHalf.z);
            cards.push_back(card(Vec3{-kHalf.x, y, z}, Vec3{1.0F, 0.0F, 0.0F}, 3));
            cards.push_back(card(Vec3{kHalf.x, y, z}, Vec3{-1.0F, 0.0F, 0.0F}, 4));
        }
        for (u32 ix = 0; ix < steps(kHalf.x); ++ix) {
            const f32 x = at(ix, kHalf.x);
            cards.push_back(card(Vec3{x, y, -kHalf.z}, Vec3{0.0F, 0.0F, 1.0F}, 5));
            cards.push_back(card(Vec3{x, y, kHalf.z}, Vec3{0.0F, 0.0F, -1.0F}, 6));
        }
    }
    return cards;
}

/// The ceiling's nine central cards, glowing: a 1.5 m square panel, 2.25 m^2.
std::vector<Surfel> with_panel(Vec3 radiance) {
    std::vector<Surfel> cards = room_cards();
    for (Surfel& surfel : cards) {
        if (surfel.normal.y < -0.5F && std::abs(surfel.position.x) <= 0.51F &&
            std::abs(surfel.position.z) <= 0.51F) {
            surfel.emission = radiance;
            surfel.instance_id = kPanelInstance;
            surfel.material_id = kPanelInstance;
        }
    }
    return cards;
}

/// A 30 cm lamp under the ceiling, between four of its cards: 0.09 m^2, far smaller than the
/// region of the ceiling it is the nearest card for.
constexpr Vec3 kLampCentre{0.25F, 0.99F, 0.25F};
constexpr f32 kLampSide = 0.3F;

std::vector<Surfel> with_lamp(Vec3 radiance) {
    std::vector<Surfel> cards = room_cards();
    Surfel lamp = card(kLampCentre, Vec3{0.0F, -1.0F, 0.0F}, kLampInstance);
    lamp.area = kLampSide * kLampSide;
    lamp.emission = radiance;
    cards.push_back(lamp);
    return cards;
}

/// The room as slabs, for the path tracer, with the lamp's own thin box when there is one.
void build_slabs(BoxProxyScene& slabs, bool lamp) {
    const f32 t = 0.5F;
    const auto add = [&](Vec3 lo, Vec3 hi) {
        CY_REQUIRE(slabs.add(ProxyBox{Aabb::from_min_max(lo, hi), kGrey}).has_value());
    };
    add({-kHalf.x - t, -kHalf.y - t, -kHalf.z - t}, {kHalf.x + t, -kHalf.y, kHalf.z + t});
    add({-kHalf.x - t, kHalf.y, -kHalf.z - t}, {kHalf.x + t, kHalf.y + t, kHalf.z + t});
    add({-kHalf.x - t, -kHalf.y, -kHalf.z - t}, {-kHalf.x, kHalf.y, kHalf.z + t});
    add({kHalf.x, -kHalf.y, -kHalf.z - t}, {kHalf.x + t, kHalf.y, kHalf.z + t});
    add({-kHalf.x, -kHalf.y, -kHalf.z - t}, {kHalf.x, kHalf.y, -kHalf.z});
    add({-kHalf.x, -kHalf.y, kHalf.z}, {kHalf.x, kHalf.y, kHalf.z + t});
    if (lamp) {
        const f32 h = kLampSide * 0.5F;
        add({kLampCentre.x - h, kLampCentre.y, kLampCentre.z - h},
            {kLampCentre.x + h, kHalf.y, kLampCentre.z + h});
    }
}

/// The emitter classification of `cards`, and what it leaves for the cache.
struct Classified {
    Array<Surfel> cache;
    Array<GiLight> lights;
    Array<EmitterRecord> records;
    EmissiveReport report{};

    Classified(const std::vector<Surfel>& cards, const EmissiveSettings& settings) {
        CY_REQUIRE(cy::rendering::gi::classify_emitters({cards.data(), cards.size()}, settings, 1,
                                                        100, cache, lights, records, report)
                       .has_value());
    }
};

/// The real-time half: the stage 2 card lighting on the host — a surface cache shaded with the
/// promoted lights through the field's occlusion, gathering through a snapshot of itself.
struct CardRoom {
    gi_support::RoomField room{kHalf};
    DistanceField field;
    SurfaceCache cache;
    SoftwareTracer occluder;

    explicit CardRoom(Span<const Surfel> cards) {
        ClipmapSettings settings;
        settings.levels = 1;
        settings.resolution = 64;
        settings.base_extent_metres = 8.0F;
        CY_REQUIRE(field.configure(settings).has_value());
        CY_REQUIRE(field.place(1, room.asset(), cy::Mat4::identity()).has_value());
        (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
        for (const Surfel& surfel : cards) {
            CY_REQUIRE(cache.allocate(surfel).has_value());
        }
        cache.set_lookup_radius(0.45F);
        occluder.bind(field, &cache);
    }

    /// `frames` updates of every card: each one gathers last update's radiance, so `frames`
    /// updates carry paths of up to `frames - 1` reflections.
    void run(Span<const GiLight> lights, u32 rays, u32 frames) {
        for (u32 frame = 1; frame <= frames; ++frame) {
            CardSnapshot snapshot;
            CY_REQUIRE(snapshot.capture(cache, frame).has_value());
            CardGather gather;
            gather.bind(&field, &snapshot, CardGatherSettings{rays, 12.0F, dark_sky()});
            SurfaceUpdateContext context;
            context.lights = lights;
            context.occluder = &occluder;
            context.indirect = &gather;
            context.frame = frame;
            (void)cache.update_all(context);
        }
    }

    /// The page at a card, by position and normal.
    [[nodiscard]] const SurfacePage& page_at(Vec3 position, Vec3 normal) const {
        for (const SurfacePage& page : cache.pages()) {
            if (page.live && cy::distance(page.position, position) < 1.0e-4F &&
                dot(page.normal, normal) > 0.99F) {
                return page;
            }
        }
        CY_REQUIRE_MESSAGE(false, "no card at the receiver");
        return cache.pages()[0];
    }

    /// The radiance arriving at a card, as the path tracer's `irradiance` measures it: the direct
    /// term plus the gathered indirect, which the page holds multiplied by its albedo.
    [[nodiscard]] Vec3 incoming(Vec3 position, Vec3 normal) const {
        const SurfacePage& page = page_at(position, normal);
        return page.direct + Vec3{page.accumulated.x / page.albedo.x,
                                  page.accumulated.y / page.albedo.y,
                                  page.accumulated.z / page.albedo.z};
    }
};

/// The reference: the same cards in a GI scene, traced through the slabs.
struct Reference {
    BoxProxyScene slabs;
    GiScene scene;

    Reference(const std::vector<Surfel>& cards, bool lamp) {
        build_slabs(slabs, lamp);
        const Aabb bounds = Aabb::from_min_max(Vec3{-4.0F, -4.0F, -4.0F}, Vec3{4.0F, 4.0F, 4.0F});
        CY_REQUIRE(scene.ingest_cell(1, bounds, {cards.data(), cards.size()}, 0).has_value());
    }

    [[nodiscard]] Vec3 irradiance(Vec3 position, Vec3 normal, u32 bounces, u32 samples,
                                  u32 seed) const {
        PathTracer tracer(slabs, scene, {}, dark_sky(), &slabs);
        tracer.set_emission_footprints(true);
        BakeSettings settings;
        settings.bounces = bounces;
        settings.samples = samples;
        settings.seed = seed;
        settings.max_distance_metres = 12.0F;
        return tracer.irradiance(position, normal, settings);
    }
};

struct Receiver {
    Vec3 position;
    Vec3 normal;
};

/// Sixteen floor cards, the centre of each wall, and two cards high on a wall.
std::vector<Receiver> room_receivers() {
    std::vector<Receiver> receivers;
    for (const f32 x : {-1.5F, -0.5F, 0.5F, 1.5F}) {
        for (const f32 z : {-1.5F, -0.5F, 0.5F, 1.5F}) {
            receivers.push_back({Vec3{x, -kHalf.y, z}, Vec3{0.0F, 1.0F, 0.0F}});
        }
    }
    receivers.push_back({Vec3{-kHalf.x, 0.0F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F}});
    receivers.push_back({Vec3{kHalf.x, 0.0F, 0.0F}, Vec3{-1.0F, 0.0F, 0.0F}});
    receivers.push_back({Vec3{0.0F, 0.0F, -kHalf.z}, Vec3{0.0F, 0.0F, 1.0F}});
    receivers.push_back({Vec3{0.0F, 0.0F, kHalf.z}, Vec3{0.0F, 0.0F, -1.0F}});
    // And high on two walls, where a shadow ray toward the ceiling arrives at a grazing angle: the
    // receivers `kEmitterShadowLiftMetres` exists for.
    receivers.push_back({Vec3{-kHalf.x, 0.5F, 0.0F}, Vec3{1.0F, 0.0F, 0.0F}});
    receivers.push_back({Vec3{0.0F, 0.5F, kHalf.z}, Vec3{0.0F, 0.0F, -1.0F}});
    return receivers;
}

/// A 5 x 5 grid of floor cards under the lamp.
std::vector<Receiver> floor_grid() {
    std::vector<Receiver> receivers;
    for (u32 ix = 0; ix < 5; ++ix) {
        for (u32 iz = 0; iz < 5; ++iz) {
            receivers.push_back({Vec3{-1.0F + (0.5F * static_cast<f32>(ix)), -kHalf.y,
                                      -1.0F + (0.5F * static_cast<f32>(iz))},
                                 Vec3{0.0F, 1.0F, 0.0F}});
        }
    }
    return receivers;
}

f32 luminance(Vec3 value) noexcept {
    return cy::rendering::gi::emission_luminance(value);
}

/// What the comparison of a real-time set against its reference comes to.
struct Agreement {
    /// |sum(measured) - sum(reference)| / sum(reference), in luminance: the energy the set
    /// receives. Reference noise averages out of it.
    f32 energy_error = 0.0F;
    /// Mean |measured - reference| / mean reference, per receiver: the shape, reference noise
    /// included.
    f32 mean_relative_error = 0.0F;
    /// Variance of measured / reference over the receivers: one for a perfect estimator plus the
    /// reference's own noise, and what a firefly blows up.
    f32 ratio_variance = 0.0F;
    /// The largest and the smallest measured / reference.
    f32 max_ratio = 0.0F;
    f32 min_ratio = 1.0e9F;
};

Agreement agree(const std::vector<Vec3>& measured, const std::vector<Vec3>& reference) {
    Agreement result;
    f64 sum_measured = 0.0;
    f64 sum_reference = 0.0;
    f64 sum_absolute = 0.0;
    f64 sum_ratio = 0.0;
    f64 sum_ratio_squared = 0.0;
    for (usize index = 0; index < measured.size(); ++index) {
        const f64 m = luminance(measured[index]);
        const f64 r = luminance(reference[index]);
        sum_measured += m;
        sum_reference += r;
        sum_absolute += std::abs(m - r);
        const f64 ratio = m / std::max(r, 1.0e-9);
        sum_ratio += ratio;
        sum_ratio_squared += ratio * ratio;
        result.max_ratio = std::max(result.max_ratio, static_cast<f32>(ratio));
        result.min_ratio = std::min(result.min_ratio, static_cast<f32>(ratio));
    }
    const auto count = static_cast<f64>(measured.size());
    result.energy_error = static_cast<f32>(std::abs(sum_measured - sum_reference) / sum_reference);
    result.mean_relative_error = static_cast<f32>(sum_absolute / sum_reference);
    const f64 mean_ratio = sum_ratio / count;
    result.ratio_variance =
        static_cast<f32>((sum_ratio_squared / count) - (mean_ratio * mean_ratio));
    return result;
}

void print(const char* label, const Agreement& agreement) {
    std::printf(
        "  %s\n    energy error %.4f   mean relative %.4f   ratio variance %.5f   ratio "
        "%.3f to %.3f\n",
        label, static_cast<double>(agreement.energy_error),
        static_cast<double>(agreement.mean_relative_error),
        static_cast<double>(agreement.ratio_variance), static_cast<double>(agreement.min_ratio),
        static_cast<double>(agreement.max_ratio));
}

/// Every setting that would promote, off: the classification sends every emitter to the cache.
EmissiveSettings cache_only() noexcept {
    EmissiveSettings settings;
    settings.promotion_power = std::numeric_limits<f32>::infinity();
    return settings;
}

/// `emitter_form_factor` by quadrature over the card: the cosine at both ends over the squared
/// distance, an n x n midpoint rule, the receiver's cosine clamped at its horizon.
f32 quadrature_form_factor(Vec3 centre, Vec3 emitter_normal, f32 area, Vec3 position, Vec3 normal,
                           u32 n) {
    Vec3 tangent;
    Vec3 bitangent;
    cy::rendering::gi::emitter_basis(emitter_normal, tangent, bitangent);
    const f32 side = std::sqrt(area);
    f64 total = 0.0;
    for (u32 i = 0; i < n; ++i) {
        for (u32 j = 0; j < n; ++j) {
            const f32 u = (((static_cast<f32>(i) + 0.5F) / static_cast<f32>(n)) - 0.5F) * side;
            const f32 v = (((static_cast<f32>(j) + 0.5F) / static_cast<f32>(n)) - 0.5F) * side;
            const Vec3 point = centre + (tangent * u) + (bitangent * v);
            const Vec3 offset = point - position;
            const f32 d2 = cy::length_squared(offset);
            const Vec3 dir = offset / std::sqrt(d2);
            const f32 receiver = std::max(0.0F, cy::dot(normal, dir));
            const f32 emitter = std::max(0.0F, -cy::dot(emitter_normal, dir));
            total += static_cast<f64>(receiver * emitter / d2);
        }
    }
    const f64 cell = static_cast<f64>(area) / static_cast<f64>(n * n);
    return static_cast<f32>(total * cell / std::numbers::pi);
}

}  // namespace

// --- The form factor ----------------------------------------------------------------------------

CY_TEST_CASE(
    "an emitter card's form factor is the closed form, the far-field limit and zero behind") {
    const Vec3 down{0.0F, -1.0F, 0.0F};
    const Vec3 up{0.0F, 1.0F, 0.0F};
    // A square of side 2a, parallel and centred above the receiver at height h: the four corner
    // rectangles of the textbook formula, summed.
    for (const f32 h : {0.25F, 1.0F, 3.0F}) {
        const f32 a = 0.5F;
        const f32 x = a / h;
        const f32 s = x / std::sqrt(1.0F + (x * x));
        const f32 exact = (4.0F / std::numbers::pi_v<f32>)*s * std::atan(s);
        const f32 computed = cy::rendering::gi::emitter_form_factor(Vec3{0.0F, h, 0.0F}, down,
                                                                    4.0F * a * a, Vec3{}, up);
        CY_TEST_INFO("height " << h);
        CY_CHECK_NEAR_REL(computed, exact, 1.0e-4F);
    }
    // Far away it is A / (pi d^2), and atan2 keeps that precision where acos would not.
    const f32 far =
        cy::rendering::gi::emitter_form_factor(Vec3{0.0F, 20.0F, 0.0F}, down, 0.01F, Vec3{}, up);
    CY_CHECK_NEAR_REL(far, 0.01F / (std::numbers::pi_v<f32> * 400.0F), 1.0e-3F);
    // One-sided: a receiver behind the card, or facing away from it, receives nothing.
    CY_CHECK_EQ(
        cy::rendering::gi::emitter_form_factor(Vec3{0.0F, -1.0F, 0.0F}, down, 1.0F, Vec3{}, up),
        0.0F);
    CY_CHECK_EQ(
        cy::rendering::gi::emitter_form_factor(Vec3{0.0F, 1.0F, 0.0F}, down, 1.0F, Vec3{}, down),
        0.0F);
    CY_CHECK_EQ(
        cy::rendering::gi::emitter_form_factor(Vec3{0.0F, 1.0F, 0.0F}, down, 0.0F, Vec3{}, up),
        0.0F);
}

CY_TEST_CASE("an emitter the receiver's horizon cuts through is clipped, not counted whole") {
    // A wall card beside a ceiling panel that reaches past the wall's plane: half of the panel is
    // behind the receiver's horizon. The closed form over the clipped polygon against a 400 x 400
    // quadrature of the cosine-weighted integrand, and against the unclipped form factor, which
    // is what a missing clip would return.
    const Vec3 centre{0.0F, 1.0F, 0.0F};
    const Vec3 down{0.0F, -1.0F, 0.0F};
    for (const Vec3 normal : {Vec3{1.0F, 0.0F, 0.0F}, cy::normalize(Vec3{1.0F, 1.0F, 0.0F}),
                              cy::normalize(Vec3{1.0F, -0.3F, 0.2F})}) {
        const Vec3 position{0.0F, 0.2F, 0.3F};
        const f32 closed =
            cy::rendering::gi::emitter_form_factor(centre, down, 1.0F, position, normal);
        const f32 numeric = quadrature_form_factor(centre, down, 1.0F, position, normal, 400);
        CY_TEST_INFO("normal " << normal.x << "," << normal.y << "," << normal.z);
        CY_CHECK_GT(numeric, 0.0F);
        CY_CHECK_NEAR_REL(closed, numeric, 2.0e-3F);
    }
}

CY_TEST_CASE("a shadow ray toward a recessed emitter is not answered by the surface around it") {
    // A lamp recessed 5 cm into the ceiling of support.h's room, seen from high on a wall with
    // nothing in between. The field's quarter-metre voxels do not resolve the recess, so the lamp's
    // own position is inside solid as far as the field knows (measured: distance -0.05), and a
    // shadow ray that ended ON the lamp is answered by the ceiling. It ends
    // `kEmitterShadowLiftMetres` in front of it instead.
    gi_support::RoomField shape{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    DistanceField field;
    CY_REQUIRE(field.configure(gi_support::room_settings().field).has_value());
    CY_REQUIRE(field.place(1, shape.asset(), cy::Mat4::identity()).has_value());
    (void)field.scroll_to(Vec3{0.0F, 0.0F, 0.0F});
    SoftwareTracer occluder;
    occluder.bind(field, nullptr);

    GiLight lamp;
    lamp.position = Vec3{0.0F, gi_support::kRoomY + 0.05F, 0.0F};
    lamp.emitter_normal = Vec3{0.0F, -1.0F, 0.0F};
    lamp.emitter_area = 0.09F;
    lamp.colour = Vec3{100.0F, 100.0F, 100.0F};
    for (const f32 height : {1.0F, 1.4F, 1.7F}) {
        const Vec3 wall{-gi_support::kRoomX, height, 0.0F};
        const Vec3 normal{1.0F, 0.0F, 0.0F};
        const Vec3 open = cy::rendering::gi::direct_radiance(lamp, wall, normal);
        const Vec3 shadowed = cy::rendering::gi::shaded_direct({&lamp, 1}, wall, normal, &occluder);
        CY_TEST_INFO("height " << height);
        CY_CHECK_GT(open.x, 0.0F);
        CY_CHECK_EQ(shadowed.x, open.x);
    }
}

// --- The classification --------------------------------------------------------------------------

CY_TEST_CASE("emitters are classified by radiant power and area, and every watt is accounted for") {
    std::vector<Surfel> cards = with_panel(Vec3{0.05F, 0.05F, 0.05F});  // a large, dim sign
    for (Surfel& surfel : cards) {
        if (surfel.instance_id == 3) {
            surfel.emission = Vec3{2.0F, 2.0F, 2.0F};  // a whole wall, bright: large, so cached
        }
    }
    Surfel lamp = card(kLampCentre, Vec3{0.0F, -1.0F, 0.0F}, kLampInstance);
    lamp.area = 0.09F;
    lamp.emission = Vec3{400.0F, 380.0F, 350.0F};  // small and bright
    cards.push_back(lamp);
    Surfel pilot = card(Vec3{1.0F, 0.99F, 1.0F}, Vec3{0.0F, -1.0F, 0.0F}, 11);
    pilot.area = 0.0004F;
    pilot.emission = Vec3{2.0F, 0.0F, 0.0F};  // small and dim: an indicator light
    cards.push_back(pilot);

    const Classified classified(cards, EmissiveSettings{});
    CY_REQUIRE_EQ(classified.records.size(), 4U);
    for (const EmitterRecord& record : classified.records) {
        CY_TEST_INFO("instance " << record.instance_id);
        const EmitterClass expected = record.instance_id == kLampInstance
                                          ? EmitterClass::PromotedLight
                                          : EmitterClass::CacheOnly;
        CY_CHECK_EQ(record.classification, expected);
    }

    // The lamp is one light that carries exactly the card: its area, normal and radiance.
    CY_REQUIRE_EQ(classified.lights.size(), 1U);
    const GiLight& light = classified.lights[0];
    CY_CHECK_EQ(light.emitter_area, 0.09F);
    CY_CHECK_EQ(light.colour.x, 400.0F);
    CY_CHECK_EQ(light.intensity, 1.0F);
    CY_CHECK_EQ(light.emitter_normal.y, -1.0F);
    CY_CHECK_EQ(light.range, 0.0F);
    CY_CHECK_FALSE(light.directional);

    // And its emission left the cache — counted once, by the light — while the sign's and the
    // indicator's stayed.
    u32 emissive_in_cache = 0;
    for (const Surfel& stored : classified.cache) {
        if (stored.instance_id == kLampInstance) {
            CY_CHECK_EQ(stored.emission.x, 0.0F);
        }
        emissive_in_cache += stored.emission.x > 0.0F ? 1U : 0U;
    }
    CY_CHECK_EQ(emissive_in_cache, 10U + 45U);
    CY_CHECK_EQ(classified.cache.size(), cards.size());

    // The energy is redistributed, never clamped: every watt is in exactly one column.
    const EmissiveReport& report = classified.report;
    CY_CHECK_EQ(report.emitters, 4U);
    CY_CHECK_EQ(report.promoted_emitters, 1U);
    CY_CHECK_EQ(report.cached_emitters, 3U);
    CY_CHECK_EQ(report.clamped_power, 0.0F);
    CY_CHECK_NEAR_REL(report.cached_power + report.promoted_power + report.demoted_power,
                      report.total_power, 1.0e-6F);
    CY_CHECK_NEAR_REL(
        report.promoted_power,
        std::numbers::pi_v<f32> * cy::rendering::gi::emission_luminance(lamp.emission) * 0.09F,
        1.0e-5F);
}

CY_TEST_CASE("an emitter over the light budget is demoted to the cache and its power reported") {
    // Two lamps, a budget of one light: the stronger is promoted, the weaker goes back to the cache
    // with its emission intact and its power in the demoted column — not lost, not clamped.
    std::vector<Surfel> cards = room_cards();
    Surfel strong = card(Vec3{-0.75F, 0.99F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F}, 20);
    strong.area = 0.04F;
    strong.emission = Vec3{500.0F, 500.0F, 500.0F};
    Surfel weak = strong;
    weak.position = Vec3{0.75F, 0.99F, 0.0F};
    weak.emission = Vec3{300.0F, 300.0F, 300.0F};
    weak.instance_id = 21;
    weak.material_id = 21;
    cards.push_back(weak);
    cards.push_back(strong);

    EmissiveSettings settings;
    settings.max_promoted_lights = 1;
    const Classified classified(cards, settings);
    CY_REQUIRE_EQ(classified.lights.size(), 1U);
    CY_CHECK_EQ(classified.lights[0].colour.x, 500.0F);
    CY_CHECK_EQ(classified.report.demoted_emitters, 1U);
    CY_CHECK_NEAR_REL(classified.report.demoted_power, cy::rendering::gi::emitted_power(weak),
                      1.0e-6F);
    CY_CHECK_EQ(classified.report.clamped_power, 0.0F);
    bool weak_kept = false;
    for (const Surfel& stored : classified.cache) {
        weak_kept = weak_kept || (stored.instance_id == 21 && stored.emission.x == 300.0F);
    }
    CY_CHECK(weak_kept);
}

// --- Against the path tracer ---------------------------------------------------------------------

CY_TEST_CASE("a glowing panel promoted to lights lights the room within 5% of the path tracer") {
    // No light placed: the panel is the only source. It is 2.25 m^2 and promoted (power 22.8),
    // so its direct term is the closed form and only the bounce is gathered.
    const std::vector<Surfel> cards = with_panel(Vec3{3.0F, 3.0F, 3.0F});
    const Classified classified(cards, EmissiveSettings{});
    CY_REQUIRE_EQ(classified.report.promoted_emitters, 1U);
    CY_REQUIRE_EQ(classified.lights.size(), 9U);

    CardRoom room(classified.cache.span());
    room.run(classified.lights.span(), 64, 6);
    const Reference reference(cards, false);

    std::vector<Vec3> measured;
    std::vector<Vec3> expected;
    u32 seed = 1;
    for (const Receiver& receiver : room_receivers()) {
        measured.push_back(room.incoming(receiver.position, receiver.normal));
        expected.push_back(reference.irradiance(receiver.position, receiver.normal, 5, 1024,
                                                0x9E3779B9U + (seed++ * 7919U)));
    }
    const Agreement agreement = agree(measured, expected);
    print("panel, promoted, 22 receivers, path tracer 1024 samples x 5 bounces", agreement);
    // MEASURED: energy 0.015; mean relative 0.070, which is the reference's own noise at 1024
    // samples per receiver.
    CY_CHECK_LT(agreement.energy_error, 0.05F);
    CY_CHECK_LT(agreement.mean_relative_error, 0.12F);
}

CY_TEST_CASE("a glowing panel left to the cache still lights the room it is in") {
    // The neon sign: the same panel with promotion off lights the room through the surface cache
    // alone — no light placed, no light promoted — and the gather's error against the path tracer
    // is stated beside the promoted one's.
    const std::vector<Surfel> cards = with_panel(Vec3{3.0F, 3.0F, 3.0F});
    const Classified classified(cards, cache_only());
    CY_REQUIRE_EQ(classified.lights.size(), 0U);
    CY_REQUIRE_EQ(classified.report.cached_emitters, 1U);

    CardRoom room(classified.cache.span());
    room.run({}, 64, 6);
    const Reference reference(cards, false);

    std::vector<Vec3> measured;
    std::vector<Vec3> expected;
    u32 seed = 1;
    for (const Receiver& receiver : room_receivers()) {
        measured.push_back(room.incoming(receiver.position, receiver.normal));
        expected.push_back(reference.irradiance(receiver.position, receiver.normal, 5, 1024,
                                                0x9E3779B9U + (seed++ * 7919U)));
    }
    for (const Vec3& value : measured) {
        CY_CHECK_GT(luminance(value), 0.0F);
    }
    const Agreement agreement = agree(measured, expected);
    print("panel, cache only, 22 receivers, 64 gather rays", agreement);
    // MEASURED: energy 0.071, mean relative 0.130 (the promoted panel: 0.015 and 0.070).
    CY_CHECK_LT(agreement.energy_error, 0.10F);
    CY_CHECK_LT(agreement.mean_relative_error, 0.20F);
}

CY_TEST_CASE("a tiny bright emitter is promoted and lights the floor without fireflies") {
    // A 30 cm lamp at radiance 400 (power 113) under the ceiling. Measured: the light arriving
    // DIRECTLY from it at 25 floor cards — the path tracer with no bounce, 16384 samples each,
    // against one update of the card lighting — once promoted and once left to the cache.
    const std::vector<Surfel> cards = with_lamp(Vec3{400.0F, 400.0F, 400.0F});
    const Reference reference(cards, true);
    std::vector<Vec3> expected;
    u32 seed = 1;
    for (const Receiver& receiver : floor_grid()) {
        expected.push_back(reference.irradiance(receiver.position, receiver.normal, 0, 16384,
                                                0x9E3779B9U + (seed++ * 7919U)));
    }

    const Classified promoted(cards, EmissiveSettings{});
    CY_REQUIRE_EQ(promoted.lights.size(), 1U);
    CardRoom lit(promoted.cache.span());
    lit.run(promoted.lights.span(), 64, 1);
    std::vector<Vec3> direct;
    for (const Receiver& receiver : floor_grid()) {
        direct.push_back(lit.page_at(receiver.position, receiver.normal).direct);
    }
    const Agreement light = agree(direct, expected);
    print("tiny lamp, promoted, 25 floor cards, direct", light);

    // The same lamp left to the cache: the second update's gather is exactly the light arriving
    // from emitters — the first gathered a snapshot with no shaded card in it, and after it every
    // card's outgoing radiance is its emission alone.
    const Classified cached(cards, cache_only());
    CY_REQUIRE_EQ(cached.lights.size(), 0U);
    CardRoom gathered(cached.cache.span());
    gathered.run({}, 64, 2);
    std::vector<Vec3> injected;
    for (const Receiver& receiver : floor_grid()) {
        injected.push_back(gathered.incoming(receiver.position, receiver.normal));
    }
    const Agreement cache = agree(injected, expected);
    print("tiny lamp, cache only, 25 floor cards, first gather", cache);

    // Promoted: the energy within 5% of the path tracer (measured 0.013) and a ratio variance at
    // the level of the reference's own noise at these sample counts (measured 0.018; at 262 144
    // samples it falls to 0.001 and the energy error to 0.003, so it is the reference that is
    // noisy, not the light). No receiver over 1.5 times its reference: no fireflies.
    CY_CHECK_LT(light.energy_error, 0.05F);
    CY_CHECK_LT(light.ratio_variance, 0.05F);
    CY_CHECK_LT(light.max_ratio, 1.5F);
    // Left to the cache, the same lamp is the firefly field the requirement is written against —
    // measured ratio variance 3.7, a receiver at 6.4 times its reference, and the energy 77% off
    // because every gather hit in the region of the ceiling the lamp is nearest to resolves to it.
    // The comparison is what makes the bound above mean something.
    CY_CHECK_GT(cache.ratio_variance, 20.0F * light.ratio_variance);
}

// --- Through the composed system -----------------------------------------------------------------

CY_TEST_CASE(
    "the composed system promotes an ingested lamp, invalidates what it lights, and forgets it on "
    "eviction") {
    using cy::rendering::gi::FrameContext;
    using cy::rendering::gi::IlluminationSettings;
    using cy::rendering::gi::IlluminationSystem;
    using cy::rendering::gi::InvalidationCause;

    IlluminationSystem system;
    IlluminationSettings settings = gi_support::room_settings();
    CY_REQUIRE(system.configure(settings).has_value());
    const std::vector<Surfel> cards = with_lamp(Vec3{400.0F, 400.0F, 400.0F});
    const Aabb bounds = Aabb::from_min_max(Vec3{-3.0F, -2.0F, -3.0F}, Vec3{3.0F, 2.0F, 3.0F});
    CY_REQUIRE(system.ingest_cell(7, bounds, {cards.data(), cards.size()}, 1).has_value());

    // The lamp is a light, attributed by its id, and its card in the scene has no emission left.
    CY_REQUIRE_EQ(system.emitters().lights().size(), 1U);
    const u64 id = system.emitters().lights()[0].id;
    CY_CHECK_EQ(system.scene().invalidation_count(InvalidationCause::LightChanged), 1U);
    bool attributed = false;
    for (const auto& record : system.scene().invalidations()) {
        attributed = attributed || (record.cause == InvalidationCause::LightChanged &&
                                    record.source_id == id && record.region.contains(Vec3{}));
    }
    CY_CHECK(attributed);
    const u32 slot = system.scene().nearest_surfel(kLampCentre, Vec3{0.0F, -1.0F, 0.0F}, 0.01F);
    CY_REQUIRE_NE(slot, GiScene::kInvalidSurfel);
    CY_CHECK_EQ(system.scene().surfel(slot).emission.x, 0.0F);
    CY_CHECK_EQ(system.surfaces().page_count(), static_cast<u32>(cards.size()));

    // A frame shades with the frame's lights and the lamp.
    const std::vector<GiLight> placed = gi_support::room_lights();
    FrameContext frame;
    frame.lights = {placed.data(), placed.size()};
    frame.frame = 2;
    (void)system.update(frame);
    CY_REQUIRE_EQ(system.frame_lights().size(), 2U);
    CY_CHECK_EQ(system.frame_lights()[0].id, placed[0].id);
    CY_CHECK_EQ(system.frame_lights()[1].id, id);

    // Evicted, it leaves the frame and invalidates what it lit once more.
    system.evict_cell(7, 3);
    CY_CHECK(system.emitters().lights().empty());
    CY_CHECK_EQ(system.scene().invalidation_count(InvalidationCause::LightChanged), 2U);
    frame.frame = 4;
    (void)system.update(frame);
    CY_CHECK_EQ(system.frame_lights().size(), 1U);
}

CY_TEST_CASE("with nothing emissive the frame's lights are the frame's own span, untouched") {
    using cy::rendering::gi::FrameContext;
    using cy::rendering::gi::IlluminationSystem;
    using cy::rendering::gi::SurfaceProperties;

    // Two systems over one room: one fed through `ingest_cell`, the other through the scene and
    // the cache directly as before. With no emitter the classification is a copy, and the two
    // resolve every point to the same bits.
    const std::vector<Surfel> cards = room_cards();
    const Aabb bounds = Aabb::from_min_max(Vec3{-3.0F, -2.0F, -3.0F}, Vec3{3.0F, 2.0F, 3.0F});
    const std::vector<GiLight> placed = gi_support::room_lights();
    gi_support::RoomField shape{kHalf};

    IlluminationSystem classified;
    IlluminationSystem direct;
    for (IlluminationSystem* system : {&classified, &direct}) {
        CY_REQUIRE(system->configure(gi_support::room_settings()).has_value());
        CY_REQUIRE(system->field().place(1, shape.asset(), cy::Mat4::identity()).has_value());
    }
    CY_REQUIRE(classified.ingest_cell(1, bounds, {cards.data(), cards.size()}, 0).has_value());
    CY_REQUIRE(direct.scene().ingest_cell(1, bounds, {cards.data(), cards.size()}, 0).has_value());
    CY_REQUIRE(direct.surfaces().allocate_from(direct.scene(), bounds).has_value());
    CY_CHECK(classified.emitters().records().empty());
    CY_CHECK_EQ(
        classified.scene().invalidation_count(cy::rendering::gi::InvalidationCause::LightChanged),
        0U);

    FrameContext frame;
    frame.lights = {placed.data(), placed.size()};
    for (u64 index = 1; index <= 3; ++index) {
        frame.frame = index;
        (void)classified.update(frame);
        (void)direct.update(frame);
    }
    CY_CHECK_EQ(classified.frame_lights().data(), placed.data());
    for (const Receiver& receiver : room_receivers()) {
        const Vec3 a =
            classified.indirect_diffuse(receiver.position, receiver.normal, SurfaceProperties{})
                .radiance;
        const Vec3 b =
            direct.indirect_diffuse(receiver.position, receiver.normal, SurfaceProperties{})
                .radiance;
        CY_CHECK_EQ(a.x, b.x);
        CY_CHECK_EQ(a.y, b.y);
        CY_CHECK_EQ(a.z, b.z);
    }
}
