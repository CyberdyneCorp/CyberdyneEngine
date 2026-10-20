// SPDX-License-Identifier: MIT
// Sunlight through a window, as the surface cache's cards carry it: the sunlit room of
// `content/beauty/interior.cyshot`, rebuilt from its boxes, its cards cut by `append_mesh_cards`
// exactly as the beauty sample cuts them, and the path tracer over the same boxes as the reference.
// Issue #35.
//
// ================================================================================================
// WHAT WENT WRONG, AND WHAT THIS FILE HOLDS
// ================================================================================================
//
// `just capture-gi-interior` measured the room 0.193 from the path tracer, the surfaces the sun's
// patch of floor lights a third too dark. The patch is about 1.6 m by 1.4 m — a few half-metre
// cards each way — and a card was shadow-tested at its centre: all lit or all dark. Which cards
// were lit came down to where their centres fell, and in this room they fell so that 1.5 m^2 of lit
// cards stood for 2.3 m^2 of sunlight. Every hit on the patch then read a card that was right where
// it was lit, and the light the patch threw into the room was two thirds of the truth.
//
// A card's direct light is the mean over its square now (`card_light_visibility`). The cases below
// hold the patch's power to the sunlight that falls through the window, and the light it throws on
// the ceiling and the far wall — one bounce, the sky black, so nothing else lights them — to the
// path tracer's.

#include <cy/test/test.h>

#include <cy/core/math/matrix.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi/mesh_cards.h>
#include <cy/rendering/gi/scene.h>
#include <cy/rendering/gi/surface_cache.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

namespace {

using cy::f32;
using cy::u32;
using cy::usize;
using cy::Vec3;
using namespace cy::rendering::gi;  // NOLINT(google-build-using-namespace)

/// The shot's camera: the sample cuts cards in the space relative to it, so the card cells fall
/// where they fell in the capture.
constexpr Vec3 kCamera{0.0F, 1.55F, -0.55F};
constexpr f32 kCardSpacing = 0.5F;
constexpr f32 kLookupRadius = 0.75F;
constexpr f32 kAlbedo = 0.8F;

/// An axis-aligned box: what every piece of the room is.
struct Box {
    Vec3 min;
    Vec3 max;
};

/// A box as `interior.cyshot` places it: `origin base`, centred in x and z on `(x, z)`, its base
/// at `y`; moved into the camera's space.
[[nodiscard]] Box room_box(f32 x, f32 y, f32 z, f32 width, f32 height, f32 depth) noexcept {
    const Vec3 lo{x - (width * 0.5F), y, z - (depth * 0.5F)};
    const Vec3 hi{x + (width * 0.5F), y + height, z + (depth * 0.5F)};
    return Box{lo - kCamera, hi - kCamera};
}

/// The room: floor and ceiling slabs, the two side walls, the near wall, and the far wall round
/// its window — 1.8 m wide, from 1.0 m to 2.4 m — every one half a metre thick.
[[nodiscard]] std::vector<Box> sunlit_room() {
    return {
        room_box(0.0F, -0.5F, 2.5F, 7.0F, 0.5F, 8.0F),   // floor
        room_box(0.0F, 3.0F, 2.5F, 7.0F, 0.5F, 8.0F),    // ceiling
        room_box(3.25F, 0.0F, 2.5F, 0.5F, 3.0F, 8.0F),   // the red wall
        room_box(-3.25F, 0.0F, 2.5F, 0.5F, 3.0F, 8.0F),  // the green wall
        room_box(0.0F, 0.0F, -1.25F, 7.0F, 3.0F, 0.5F),  // the near wall
        room_box(2.2F, 0.0F, 6.25F, 2.6F, 3.0F, 0.5F),   // the far wall's piers
        room_box(-2.2F, 0.0F, 6.25F, 2.6F, 3.0F, 0.5F),
        room_box(0.0F, 0.0F, 6.25F, 1.8F, 1.0F, 0.5F),  // its sill
        room_box(0.0F, 2.4F, 6.25F, 1.8F, 0.6F, 0.5F),  // its lintel
    };
}

/// The shot's sun: 35 degrees up at azimuth 72, as the beauty stage turns `sun elevation` and
/// `sun azimuth` into a direction.
[[nodiscard]] GiLight shot_sun() noexcept {
    const f32 elevation = 35.0F * std::numbers::pi_v<f32> / 180.0F;
    const f32 azimuth = 72.0F * std::numbers::pi_v<f32> / 180.0F;
    const Vec3 to_sun{std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                      std::cos(elevation) * std::sin(azimuth)};
    GiLight sun;
    sun.directional = true;
    sun.direction = -to_sun;
    sun.colour = Vec3{1.0F, 1.0F, 1.0F};
    sun.intensity = 10.0F;
    sun.id = 1;
    return sun;
}

/// The twelve outward-wound triangles of a box.
void append_box_triangles(const Box& box, std::vector<Vec3>& positions, std::vector<u32>& indices) {
    const u32 first = static_cast<u32>(positions.size());
    for (u32 corner = 0; corner < 8; ++corner) {
        positions.push_back(Vec3{(corner & 1U) != 0U ? box.max.x : box.min.x,
                                 (corner & 2U) != 0U ? box.max.y : box.min.y,
                                 (corner & 4U) != 0U ? box.max.z : box.min.z});
    }
    // Each face as a quad (a, b, c, d) counter-clockwise seen from outside.
    constexpr u32 kFaces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4},
                                  {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    for (const auto& face : kFaces) {
        for (const u32 corner : {face[0], face[1], face[2], face[0], face[2], face[3]}) {
            indices.push_back(first + corner);
        }
    }
}

/// The room's boxes, traced exactly: the slab test against each, nearest hit first. A ray that
/// starts inside a box is occluded at once, as one inside a closed triangle mesh is.
class BoxTracer final : public SceneTracer, public Occluder {
public:
    explicit BoxTracer(std::vector<Box> boxes) : boxes_(std::move(boxes)) {}

    [[nodiscard]] bool trace(Vec3 origin, Vec3 direction, f32 max_distance,
                             SceneHit& hit) const noexcept override {
        hit = SceneHit{};
        f32 nearest = max_distance;
        for (const Box& box : boxes_) {
            f32 entry = 0.0F;
            u32 axis = 0;
            f32 sign = 0.0F;
            if (enter(box, origin, direction, nearest, entry, axis, sign)) {
                nearest = entry;
                hit.hit = true;
                hit.t = entry;
                hit.normal =
                    Vec3{axis == 0 ? sign : 0.0F, axis == 1 ? sign : 0.0F, axis == 2 ? sign : 0.0F};
            }
        }
        if (hit.hit) {
            hit.position = origin + (direction * hit.t);
        }
        return hit.hit;
    }

    [[nodiscard]] bool occluded(Vec3 from, Vec3 to) const noexcept override {
        const Vec3 offset = to - from;
        const f32 span = cy::length(offset);
        if (span <= 1.0e-4F) {
            return false;
        }
        SceneHit hit;
        return trace(from, offset / span, span, hit);
    }

private:
    /// Where a ray enters `box` before `limit`, and the face it enters through: axis and the sign
    /// of its outward normal. An origin inside the box enters at zero.
    [[nodiscard]] static bool enter(const Box& box, Vec3 origin, Vec3 direction, f32 limit,
                                    f32& entry, u32& axis, f32& sign) noexcept {
        const f32 o[3] = {origin.x, origin.y, origin.z};
        const f32 d[3] = {direction.x, direction.y, direction.z};
        const f32 lo[3] = {box.min.x, box.min.y, box.min.z};
        const f32 hi[3] = {box.max.x, box.max.y, box.max.z};
        f32 near = -1.0e30F;
        f32 far = 1.0e30F;
        for (u32 k = 0; k < 3; ++k) {
            if (std::abs(d[k]) < 1.0e-12F) {
                if (o[k] < lo[k] || o[k] > hi[k]) {
                    return false;
                }
                continue;
            }
            const f32 a = (lo[k] - o[k]) / d[k];
            const f32 b = (hi[k] - o[k]) / d[k];
            const f32 t0 = std::min(a, b);
            if (t0 > near) {
                near = t0;
                axis = k;
                sign = d[k] > 0.0F ? -1.0F : 1.0F;
            }
            far = std::min(far, std::max(a, b));
        }
        if (far < std::max(near, 0.0F) || near >= limit) {
            return false;
        }
        entry = std::max(near, 0.0F);
        return true;
    }

    std::vector<Box> boxes_;
};

/// The room carded as the beauty sample cards it, its pages shaded by the sun alone.
struct CardedRoom {
    std::vector<Box> boxes = sunlit_room();
    BoxTracer tracer{boxes};
    GiLight sun = shot_sun();
    GiScene scene;
    SurfaceCache cache;

    CardedRoom() {
        cy::Array<Surfel> cards;
        u32 instance = 0;
        for (const Box& box : boxes) {
            std::vector<Vec3> positions;
            std::vector<u32> indices;
            append_box_triangles(box, positions, indices);
            MeshCardSettings settings;
            settings.spacing_metres = kCardSpacing;
            settings.albedo = Vec3{kAlbedo, kAlbedo, kAlbedo};
            settings.instance_id = ++instance;
            CY_REQUIRE(append_mesh_cards({positions.data(), positions.size()},
                                         {indices.data(), indices.size()}, cy::Mat4::identity(),
                                         settings, cards)
                           .has_value());
        }
        const cy::Aabb bounds =
            cy::Aabb::from_min_max(Vec3{-5.0F, -3.0F, -2.0F}, Vec3{5.0F, 3.0F, 9.0F});
        CY_REQUIRE(scene.ingest_cell(1, bounds, cards.span(), 0).has_value());
        CY_REQUIRE(cache.allocate_from(scene, bounds).has_value());
        cache.set_lookup_radius(kLookupRadius);
        SurfaceUpdateContext context;
        context.lights = {&sun, 1};
        context.occluder = &tracer;
        (void)cache.update_all(context);
    }
};

[[nodiscard]] f32 luminance(Vec3 c) noexcept {
    return (0.2126F * c.x) + (0.7152F * c.y) + (0.0722F * c.z);
}

/// A cosine-weighted direction about `normal` from two numbers in [0, 1).
[[nodiscard]] Vec3 cosine_direction(Vec3 normal, f32 u1, f32 u2) noexcept {
    const f32 radius = std::sqrt(u1);
    const f32 angle = 2.0F * std::numbers::pi_v<f32> * u2;
    Vec3 tangent;
    Vec3 bitangent;
    emitter_basis(normal, tangent, bitangent);
    return cy::normalized_or((tangent * (radius * std::cos(angle))) +
                                 (bitangent * (radius * std::sin(angle))) +
                                 (normal * std::sqrt(std::max(0.0F, 1.0F - u1))),
                             normal);
}

/// The light arriving at a receiver as the caches see it: cosine rays through the exact room, each
/// hit answered by the cache's lookup — what every tier does with a hit. The sky is black.
[[nodiscard]] Vec3 cached_irradiance(const CardedRoom& room, Vec3 position, Vec3 normal) {
    constexpr u32 kEdge = 64;
    Vec3 total{0.0F, 0.0F, 0.0F};
    for (u32 row = 0; row < kEdge; ++row) {
        for (u32 column = 0; column < kEdge; ++column) {
            const Vec3 direction = cosine_direction(normal, (static_cast<f32>(row) + 0.5F) / kEdge,
                                                    (static_cast<f32>(column) + 0.5F) / kEdge);
            SceneHit hit;
            if (!room.tracer.trace(position + (normal * 1.0e-3F), direction, 40.0F, hit)) {
                continue;
            }
            Vec3 radiance{0.0F, 0.0F, 0.0F};
            u32 age = 0;
            if (room.cache.radiance_at(hit.position, hit.normal, radiance, age)) {
                total = total + radiance;
            }
        }
    }
    return total / static_cast<f32>(kEdge * kEdge);
}

/// The surfaces the patch lights: the ceiling over the room and the far wall either side of the
/// window, a hand's breadth off each, in the camera's space.
[[nodiscard]] std::vector<std::pair<Vec3, Vec3>> patch_receivers() {
    std::vector<std::pair<Vec3, Vec3>> receivers;
    for (const f32 x : {-2.0F, -1.0F, 0.0F, 1.0F, 2.0F}) {
        for (const f32 z : {1.0F, 2.5F, 4.0F, 5.5F}) {
            receivers.emplace_back(Vec3{x, 2.95F, z} - kCamera, Vec3{0.0F, -1.0F, 0.0F});
        }
    }
    for (const f32 x : {-2.4F, -1.6F, 1.6F, 2.4F}) {
        for (const f32 y : {0.5F, 1.5F, 2.5F}) {
            receivers.emplace_back(Vec3{x, y, 5.95F} - kCamera, Vec3{0.0F, 0.0F, -1.0F});
        }
    }
    return receivers;
}

}  // namespace

CY_TEST_CASE("the cards in a window's sunlight carry the power the window lets in") {
    const CardedRoom room;
    // THE TRUTH: the floor inside the room, sampled every two centimetres, each point lit or not by
    // the exact boxes — the patch's area times the sun's irradiance on the floor.
    const Vec3 up{0.0F, 1.0F, 0.0F};
    constexpr f32 kStep = 0.02F;
    f32 lit_area = 0.0F;
    // The floor is 6 m across and 7 m deep: 300 by 350 samples.
    for (u32 column = 0; column < 300U; ++column) {
        for (u32 row = 0; row < 350U; ++row) {
            const f32 x = -3.0F + ((static_cast<f32>(column) + 0.5F) * kStep);
            const f32 z = -1.0F + ((static_cast<f32>(row) + 0.5F) * kStep);
            const Vec3 at = Vec3{x, 0.0F, z} - kCamera;
            if (!room.tracer.occluded(at + (up * 1.0e-2F), shadow_target(room.sun, at))) {
                lit_area += kStep * kStep;
            }
        }
    }
    const f32 irradiance = luminance(direct_radiance(room.sun, Vec3{}, up));
    const f32 truth = lit_area * irradiance;

    // THE CACHE: every upward card on the floor, its direct light times the surface it stands for.
    f32 carried = 0.0F;
    for (const SurfacePage& page : room.cache.pages()) {
        if (page.live && page.normal.y > 0.9F && std::abs(page.position.y + kCamera.y) < 0.01F) {
            carried += luminance(page.direct) * page.area;
        }
    }
    const f32 ratio = carried / truth;
    std::printf(
        "[window light] the sunlit floor: %.2f m^2 of sunlight, the cards carry %.3f of it\n",
        static_cast<double>(lit_area), static_cast<double>(ratio));
    CY_CHECK(lit_area > 2.0F);
    // Within the share of the patch one 4x4 lattice of samples over a half-metre card can miss.
    CY_CHECK(std::abs(ratio - 1.0F) < 0.06F);
}

CY_TEST_CASE(
    "what a window's sunlit patch throws on the ceiling and the far wall is the path tracer's") {
    const CardedRoom room;
    const SkyTerm black{Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, 0.0F},
                        0.0F};
    const PathTracer tracer(room.tracer, room.scene, {&room.sun, 1}, black, &room.tracer);
    BakeSettings bake;
    // One bounce: the sun on what a path meets, reflected once. Exactly what the cards' direct
    // light answers; the multi-bounce feedback is not under test here.
    bake.bounces = 0;
    bake.samples = 4096;
    bake.max_distance_metres = 40.0F;

    std::vector<Vec3> cached;
    std::vector<Vec3> reference;
    f32 worst = 1.0F;
    for (const auto& [position, normal] : patch_receivers()) {
        cached.push_back(cached_irradiance(room, position, normal));
        reference.push_back(tracer.irradiance(position, normal, bake));
        const f32 truth = luminance(reference.back());
        if (truth > 0.0F) {
            const f32 share = luminance(cached.back()) / truth;
            worst = std::abs(share - 1.0F) > std::abs(worst - 1.0F) ? share : worst;
        }
    }
    const ReferenceComparison compared = compare_against_reference(
        {cached.data(), cached.size()}, {reference.data(), reference.size()});
    f32 cached_total = 0.0F;
    f32 reference_total = 0.0F;
    for (usize index = 0; index < cached.size(); ++index) {
        cached_total += luminance(cached[index]);
        reference_total += luminance(reference[index]);
    }
    const f32 energy = cached_total / reference_total;
    std::printf(
        "[window light] %u receivers on the ceiling and the far wall: the cards give %.3f of the "
        "path tracer's light, %.3f relative error, the furthest receiver %.3f\n",
        compared.samples, static_cast<double>(energy), static_cast<double>(compared.relative_error),
        static_cast<double>(worst));
    CY_CHECK_EQ(compared.samples, 32U);
    // Before a card's direct light was its footprint's mean, 0.69 and 0.31.
    CY_CHECK(std::abs(energy - 1.0F) < 0.05F);
    CY_CHECK(compared.relative_error < 0.08F);
}
