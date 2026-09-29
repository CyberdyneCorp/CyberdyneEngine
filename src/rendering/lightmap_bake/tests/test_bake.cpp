// SPDX-License-Identifier: MIT
// The lightmap bake over real rooms. `integration.render_lightmap_bake`.
//
// `rendering-global-illumination` — "Lightmap baking" ("Normal maps still respond", the seeds),
// "UV2 and chart packing" ("Seam artifacts"), "Offline path tracer and ground truth".
//
// Every scene is built from one quad mesh placed by transforms, so what a case asserts about a
// wall is about the bake and not about a mesh built for the case. The ground truth is
// `reference_ambient`, which builds its own tracer and surface cards and draws its own samples: it
// shares the SCENE with the bake and nothing else.

#include <cy/core/math/matrix.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/lightmap_bake/asset.h>
#include <cy/rendering/lightmap_bake/bake.h>
#include <cy/rendering/lightmap_bake/mips.h>
#include <cy/test/test.h>

#include "support.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numbers>
#include <string_view>
#include <vector>

namespace {

using namespace cy::rendering::lightmap_bake;  // NOLINT(google-build-using-namespace)
namespace gi = cy::rendering::gi;
using cy::Aabb;
using cy::f32;
using cy::f64;
using cy::Mat4;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::Vec2;
using cy::Vec3;
using cy::Vec4;

// --- Geometry ----------------------------------------------------------------------------------

/// A unit quad in the XZ plane, facing +y, UV0 and UV2 both its own square.
struct QuadMesh {
    std::vector<Vec3> positions = {
        {-0.5F, 0.0F, -0.5F}, {-0.5F, 0.0F, 0.5F}, {0.5F, 0.0F, 0.5F}, {0.5F, 0.0F, -0.5F}};
    std::vector<Vec3> normals = std::vector<Vec3>(4, Vec3{0.0F, 1.0F, 0.0F});
    std::vector<Vec2> uvs = {{0.0F, 0.0F}, {0.0F, 1.0F}, {1.0F, 1.0F}, {1.0F, 0.0F}};
    std::vector<cy::u32> indices = {0, 1, 2, 0, 2, 3};

    [[nodiscard]] BakeMesh mesh() const {
        BakeMesh out;
        out.positions = {positions.data(), positions.size()};
        out.normals = {normals.data(), normals.size()};
        out.uv0 = {uvs.data(), uvs.size()};
        out.uv2 = {uvs.data(), uvs.size()};
        out.indices = {indices.data(), indices.size()};
        return out;
    }
};

/// Two quads side by side sharing an edge in space and NOT in UV2: two charts meeting at a seam,
/// with the same normal on both sides, which is the case seam reconciliation is for. The right
/// half's chart is flipped in the atlas, so the texels either side of the seam are not neighbours.
struct SeamMesh {
    std::vector<Vec3> positions = {  // left half, x in [-0.5, 0]
        {-0.5F, 0.0F, -0.5F},
        {-0.5F, 0.0F, 0.5F},
        {0.0F, 0.0F, 0.5F},
        {0.0F, 0.0F, -0.5F},
        // right half, x in [0, 0.5]: its own four vertices
        {0.0F, 0.0F, -0.5F},
        {0.0F, 0.0F, 0.5F},
        {0.5F, 0.0F, 0.5F},
        {0.5F, 0.0F, -0.5F}};
    std::vector<Vec3> normals = std::vector<Vec3>(8, Vec3{0.0F, 1.0F, 0.0F});
    std::vector<Vec2> uv2 = {{0.05F, 0.05F},
                             {0.05F, 0.95F},
                             {0.45F, 0.95F},
                             {0.45F, 0.05F},
                             // mirrored: the seam edge (x = 0) is at u = 0.95 on this side
                             {0.95F, 0.05F},
                             {0.95F, 0.95F},
                             {0.55F, 0.95F},
                             {0.55F, 0.05F}};
    std::vector<cy::u32> indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};

    [[nodiscard]] BakeMesh mesh() const {
        BakeMesh out;
        out.positions = {positions.data(), positions.size()};
        out.normals = {normals.data(), normals.size()};
        out.uv2 = {uv2.data(), uv2.size()};
        out.indices = {indices.data(), indices.size()};
        out.uv_coverage = 0.72F;
        return out;
    }
};

/// A frame (a, n, b) with a x n = b, scaled: the quad's x along `a` by `width`, its normal along
/// `n`, its z along `b` by `depth`, centred at `centre`.
[[nodiscard]] Mat4 place(Vec3 a, Vec3 n, f32 width, f32 depth, Vec3 centre) {
    const Vec3 b = cy::cross(a, n);
    return Mat4::from_columns(cy::Vec4{a.x * width, a.y * width, a.z * width, 0.0F},
                              cy::Vec4{n.x, n.y, n.z, 0.0F},
                              cy::Vec4{b.x * depth, b.y * depth, b.z * depth, 0.0F},
                              cy::Vec4{centre.x, centre.y, centre.z, 1.0F});
}

/// Where UV2 lands in the world on a placed quad.
[[nodiscard]] Vec3 quad_point(const Mat4& transform, Vec2 uv) {
    return cy::transform_point(transform, Vec3{uv.x - 0.5F, 0.0F, uv.y - 0.5F});
}

/// A closed box room of half extents (hx, hy, hz), every face inward, one instance per face.
enum Face : u32 { kFloor = 0, kCeiling, kLeft, kRight, kBack, kFront, kFaces };

struct Room {
    QuadMesh quad;
    std::vector<BakeMesh> meshes;
    std::vector<BakeMaterial> materials;
    std::vector<BakeInstance> instances;
    std::vector<gi::GiLight> lights;
    Vec3 half{2.0F, 1.25F, 2.0F};

    explicit Room(Vec3 half_extents = Vec3{2.0F, 1.25F, 2.0F}) : half(half_extents) {
        meshes.push_back(quad.mesh());
        const Vec3 x{1, 0, 0};
        const Vec3 y{0, 1, 0};
        const Vec3 z{0, 0, 1};
        const auto add = [&](Mat4 transform) {
            BakeInstance instance;
            instance.mesh = 0;
            instance.material = static_cast<u32>(materials.size());
            instance.transform = transform;
            instance.id = instances.size();
            instances.push_back(instance);
            materials.push_back(BakeMaterial{Vec3{0.7F, 0.7F, 0.7F}});
        };
        add(place(x, y, 2 * half.x, 2 * half.z, Vec3{0, -half.y, 0}));
        add(place(x, -y, 2 * half.x, 2 * half.z, Vec3{0, half.y, 0}));
        add(place(y, x, 2 * half.y, 2 * half.z, Vec3{-half.x, 0, 0}));
        add(place(y, -x, 2 * half.y, 2 * half.z, Vec3{half.x, 0, 0}));
        add(place(x, z, 2 * half.x, 2 * half.y, Vec3{0, 0, -half.z}));
        add(place(x, -z, 2 * half.x, 2 * half.y, Vec3{0, 0, half.z}));
    }

    [[nodiscard]] LightmapScene scene() const {
        LightmapScene out;
        out.meshes = {meshes.data(), meshes.size()};
        out.materials = {materials.data(), materials.size()};
        out.instances = {instances.data(), instances.size()};
        out.lights = {lights.data(), lights.size()};
        // A closed room sees no sky; black makes that true of any ray that leaks at an edge.
        out.sky.zenith = Vec3{};
        out.sky.horizon = Vec3{};
        out.sky.ground = Vec3{};
        return out;
    }

    void point_light(Vec3 position, f32 intensity) {
        gi::GiLight light;
        light.position = position;
        light.intensity = intensity;
        light.range = 30.0F;
        light.id = lights.size() + 1;
        lights.push_back(light);
    }
};

[[nodiscard]] LightmapBakeSettings small_settings(LightmapMode mode) {
    LightmapBakeSettings settings;
    settings.mode = mode;
    settings.atlas.page_size = 128;
    settings.atlas.texel_density = 5.0F;
    settings.trace.bounces = 2;
    settings.trace.samples = 32;
    settings.trace.max_distance_metres = 20.0F;
    // A metre between cards: the path tracer looks a hit's material up within a metre, and the
    // cost of that lookup is what a bake spends most of its time on.
    settings.surfel_spacing = 1.0F;
    return settings;
}

struct Baked {
    BakedLightmap lightmap;
    LightmapBakeReport report;
};

[[nodiscard]] Baked bake(const LightmapScene& scene, const LightmapBakeSettings& settings,
                         const CacheSeedTargets* seeds = nullptr) {
    Baked out;
    const cy::Status baked = bake_lightmaps(scene, settings, seeds, out.lightmap, out.report);
    if (!baked) {
        std::fprintf(stderr, "bake failed: %s\n", baked.error().message);
    }
    CY_REQUIRE(baked.has_value());
    return out;
}

[[nodiscard]] std::vector<Vec3> reference(const LightmapScene& scene, LightmapBakeSettings settings,
                                          const std::vector<Vec3>& positions,
                                          const std::vector<Vec3>& normals, u32 samples) {
    settings.trace.samples = samples;
    cy::Array<Vec3> out;
    CY_REQUIRE(reference_ambient(scene, settings, {positions.data(), positions.size()},
                                 {normals.data(), normals.size()}, out)
                   .has_value());
    return {out.begin(), out.end()};
}

[[nodiscard]] f32 luminance(Vec3 value) {
    return (0.2126F * value.x) + (0.7152F * value.y) + (0.0722F * value.z);
}

[[nodiscard]] Vec3 tilt(Vec3 normal, Vec3 toward, f32 degrees) {
    const f32 radians = degrees * std::numbers::pi_v<f32> / 180.0F;
    return cy::normalize((normal * std::cos(radians)) + (toward * std::sin(radians)));
}

}  // namespace

// --- Ground truth ------------------------------------------------------------------------------

CY_TEST_CASE("a small room bakes to texels that match the path tracer's ground truth") {
    Room room;
    room.materials[kLeft].albedo = Vec3{0.85F, 0.1F, 0.08F};
    room.point_light(Vec3{0.8F, 0.6F, -0.4F}, 40.0F);
    const LightmapScene scene = room.scene();
    const LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    const Baked baked = bake(scene, settings);

    CY_CHECK_EQ(baked.report.objects, 6U);
    CY_CHECK_GT(baked.report.texels_covered, 1000U);
    CY_CHECK_GT(baked.report.rays, 0U);

    // Texels read back through the CPU sampler at points inside the floor and the red wall.
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec3> measured;
    for (const u32 face : {static_cast<u32>(kFloor), static_cast<u32>(kLeft)}) {
        const BakeInstance& instance = room.instances[face];
        const Vec3 normal =
            cy::normalize(cy::transform_direction(instance.transform, Vec3{0.0F, 1.0F, 0.0F}));
        for (u32 row = 0; row < 3; ++row) {
            for (u32 column = 0; column < 4; ++column) {
                const Vec2 uv{0.15F + (0.23F * static_cast<f32>(column)),
                              0.2F + (0.3F * static_cast<f32>(row))};
                positions.push_back(quad_point(instance.transform, uv));
                normals.push_back(normal);
                measured.push_back(
                    sample_lightmap(baked.lightmap, baked.lightmap.addresses[face], uv, normal));
            }
        }
    }
    const std::vector<Vec3> truth = reference(scene, settings, positions, normals, 512);
    const gi::ReferenceComparison comparison = gi::compare_against_reference(
        {measured.data(), measured.size()}, {truth.data(), truth.size()});
    CY_TEST_MESSAGE("lightmap against ground truth: relative "
                    << comparison.relative_error << ", max " << comparison.max_absolute_error
                    << " of mean " << comparison.mean_reference_magnitude);
    CY_CHECK_GT(comparison.mean_reference_magnitude, 0.01F);
    // THE STATED ERROR: a tenth of the mean, over twenty-four points at 32 samples per texel
    // denoised, against 512 samples at the point.
    CY_CHECK_LT(comparison.relative_error, 0.10F);

    // And the room is lit the way a room is: the floor beside the red wall is redder than the
    // floor across the room from it, which the bake can only know by bouncing.
    const Vec3 near_red = sample_lightmap(baked.lightmap, baked.lightmap.addresses[kFloor],
                                          Vec2{0.08F, 0.5F}, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 far_red = sample_lightmap(baked.lightmap, baked.lightmap.addresses[kFloor],
                                         Vec2{0.92F, 0.5F}, Vec3{0.0F, 1.0F, 0.0F});
    CY_CHECK_GT(near_red.x / std::max(near_red.z, 1.0e-6F),
                far_red.x / std::max(far_red.z, 1.0e-6F));
}

// --- Normal maps -------------------------------------------------------------------------------

CY_TEST_CASE("directional and SH L1 lightmaps keep a normal map's response; irradiance does not") {
    // A dark room with two emissive walls: red at +x, blue at -x. A floor texel's light arrives
    // from the sides, so a shading normal tilted toward +x should see more red.
    Room room;
    room.materials[kRight].emission = Vec3{6.0F, 0.4F, 0.3F};
    room.materials[kLeft].emission = Vec3{0.3F, 0.4F, 3.0F};
    const LightmapScene scene = room.scene();
    const Vec2 uv{0.5F, 0.5F};
    const Mat4& floor = room.instances[kFloor].transform;
    const Vec3 up{0.0F, 1.0F, 0.0F};
    const Vec3 toward = tilt(up, Vec3{1.0F, 0.0F, 0.0F}, 40.0F);
    const Vec3 away = tilt(up, Vec3{-1.0F, 0.0F, 0.0F}, 40.0F);

    const std::vector<Vec3> positions(3, quad_point(floor, uv));
    const std::vector<Vec3> normals = {up, toward, away};
    const std::vector<Vec3> truth =
        reference(scene, small_settings(LightmapMode::Irradiance), positions, normals, 2048);
    // The ground truth's own response: this scene has one for the modes to keep.
    CY_REQUIRE(luminance(truth[1]) > luminance(truth[0]) * 1.1F);
    CY_REQUIRE(truth[1].x / truth[1].z > truth[0].x / truth[0].z);

    const auto read = [&](LightmapMode mode) {
        // Coarse: one texel at the floor's centre is read, and three bakes share the case's budget.
        LightmapBakeSettings settings = small_settings(mode);
        settings.atlas.texel_density = 3.0F;
        settings.trace.samples = 16;
        const Baked baked = bake(scene, settings);
        const u32 address = baked.lightmap.addresses[kFloor];
        return std::vector<Vec3>{sample_lightmap(baked.lightmap, address, uv, up),
                                 sample_lightmap(baked.lightmap, address, uv, toward),
                                 sample_lightmap(baked.lightmap, address, uv, away)};
    };

    // IRRADIANCE-ONLY IS BLIND TO THE SHADING NORMAL, which is the reason the other two exist.
    const std::vector<Vec3> irradiance = read(LightmapMode::Irradiance);
    CY_CHECK_EQ(luminance(irradiance[1]), luminance(irradiance[0]));
    CY_CHECK_EQ(luminance(irradiance[2]), luminance(irradiance[0]));

    // DIRECTIONAL: brighter toward the bright wall, darker away, and nearer the truth at the tilted
    // normal than the irradiance-only answer is.
    const std::vector<Vec3> directional = read(LightmapMode::Directional);
    CY_CHECK_GT(luminance(directional[1]), luminance(directional[0]) * 1.05F);
    CY_CHECK_LT(luminance(directional[2]), luminance(directional[0]) * 0.95F);
    CY_CHECK_LT(std::fabs(luminance(directional[1]) - luminance(truth[1])),
                std::fabs(luminance(irradiance[1]) - luminance(truth[1])));
    // One direction for all three channels: the colour does not turn with the normal.
    CY_CHECK_NEAR(directional[1].x / directional[1].z, directional[0].x / directional[0].z,
                  1.0e-2F * directional[0].x / directional[0].z);

    // SH L1: per channel, so tilting toward the red wall turns the colour red as the truth does.
    const std::vector<Vec3> sh = read(LightmapMode::ShL1);
    const auto gain = [&](const std::vector<Vec3>& values) {
        return luminance(values[1]) / luminance(values[0]);
    };
    CY_TEST_MESSAGE("tilted 40 degrees toward the bright wall: truth x"
                    << gain(truth) << ", directional x" << gain(directional) << ", SH L1 x"
                    << gain(sh) << ", irradiance x" << gain(irradiance));
    CY_CHECK_GT(luminance(sh[1]), luminance(sh[0]) * 1.05F);
    CY_CHECK_GT(sh[1].x / sh[1].z, (sh[0].x / sh[0].z) * 1.05F);
    CY_CHECK_LT(std::fabs(luminance(sh[1]) - luminance(truth[1])),
                std::fabs(luminance(irradiance[1]) - luminance(truth[1])));
}

// --- Seams -------------------------------------------------------------------------------------

CY_TEST_CASE("texels at a seam are dilated and reconciled so the seam does not show") {
    SeamMesh seam_mesh;
    QuadMesh quad;
    std::vector<BakeMesh> meshes = {seam_mesh.mesh(), quad.mesh()};
    std::vector<BakeMaterial> materials = {BakeMaterial{Vec3{0.7F, 0.7F, 0.7F}},
                                           BakeMaterial{Vec3{0.7F, 0.7F, 0.7F}}};
    std::vector<BakeInstance> instances(2);
    instances[0].mesh = 0;
    instances[0].transform = place({1, 0, 0}, {0, 1, 0}, 4.0F, 4.0F, Vec3{});
    // A wall beside the floor so the floor receives bounce as well as sky.
    instances[1].mesh = 1;
    instances[1].material = 1;
    instances[1].transform = place({0, 1, 0}, {1, 0, 0}, 2.0F, 4.0F, Vec3{-2.0F, 1.0F, 0.0F});
    std::vector<gi::GiLight> lights(1);
    lights[0].position = Vec3{0.3F, 1.2F, 0.4F};
    lights[0].intensity = 30.0F;
    lights[0].range = 20.0F;
    LightmapScene scene;
    scene.meshes = {meshes.data(), meshes.size()};
    scene.materials = {materials.data(), materials.size()};
    scene.instances = {instances.data(), instances.size()};
    scene.lights = {lights.data(), lights.size()};

    LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    // Indirect only and not denoised, so each side of the seam carries its own sampling noise: the
    // disagreement the reconciliation exists to remove.
    settings.trace.samples = 8;
    settings.denoise = false;
    settings.atlas.texel_density = 6.0F;

    // Both sides of the seam at matched points along it: world x = 0, z from -2 to 2. The left
    // chart's seam edge is its u = 0.45, the right chart's its u = 0.95.
    const auto disagreement = [&](const BakedLightmap& lightmap) {
        f32 worst = 0.0F;
        f32 mean = 0.0F;
        for (u32 step = 0; step <= 16; ++step) {
            const f32 v = 0.05F + (0.9F * static_cast<f32>(step) / 16.0F);
            const Vec3 left =
                sample_lightmap(lightmap, lightmap.addresses[0], Vec2{0.45F, v}, Vec3{0, 1, 0});
            const Vec3 right =
                sample_lightmap(lightmap, lightmap.addresses[0], Vec2{0.95F, v}, Vec3{0, 1, 0});
            worst = std::max(worst, std::fabs(luminance(left) - luminance(right)));
            mean += luminance(left);
        }
        return worst / (mean / 17.0F);
    };

    const Baked reconciled = bake(scene, settings);
    CY_CHECK_EQ(reconciled.report.seam_edges, 1U);
    CY_CHECK_GT(reconciled.report.seam_samples, 0U);
    CY_CHECK_LT(reconciled.report.seam_error_after, reconciled.report.seam_error_before);

    settings.reconcile_seams = false;
    const Baked raw = bake(scene, settings);
    const f32 before = disagreement(raw.lightmap);
    const f32 after = disagreement(reconciled.lightmap);
    CY_TEST_MESSAGE("seam disagreement: " << before << " unreconciled, " << after
                                          << " reconciled; the solve's own "
                                          << reconciled.report.seam_error_before << " -> "
                                          << reconciled.report.seam_error_after);
    CY_CHECK_GT(before, 0.02F);
    // "No seam is visible at the bake resolution": under one 8-bit step of a mid-grey.
    CY_CHECK_LT(after, 0.01F);

    // DILATED: every texel of the floor's rectangle, gutter and chart padding included, holds
    // light, so a bilinear tap or a mip level at a chart's border never reads black.
    AtlasPlacement rectangle;
    CY_REQUIRE(decode_address(reconciled.lightmap.addresses[0], rectangle));
    const u32 block = reconciled.lightmap.page_size / kAddressBlocks;
    const u32 x0 = rectangle.block_x * block;
    const u32 y0 = (rectangle.page * reconciled.lightmap.page_size) + (rectangle.block_y * block);
    u32 dark = 0;
    u32 empty = 0;
    for (u32 y = y0; y < y0 + (rectangle.block_height * block); ++y) {
        for (u32 x = x0; x < x0 + (rectangle.block_width * block); ++x) {
            const usize index = reconciled.lightmap.texels.index(0, x, y);
            dark += reconciled.lightmap.texels.texels[index].x <= 0.0F ? 1U : 0U;
            empty +=
                reconciled.lightmap.coverage[(usize{y} * reconciled.lightmap.page_size) + x] == 0U
                    ? 1U
                    : 0U;
        }
    }
    CY_CHECK_GT(empty, 0U);  // there is padding to fill...
    CY_CHECK_EQ(dark, 0U);   // ...and all of it is filled
    CY_CHECK_GT(reconciled.report.texels_dilated, 0U);
}

CY_TEST_CASE("where two objects meet, the edge they share is a seam and is reconciled") {
    // Two 2 m floor tiles side by side, each its own object with its own rectangle: the edge at
    // x = 0 is stored twice in the atlas, in two rectangles that are not neighbours. The first
    // version looked for seams inside one object only, and the back wall of `render.lightmaps`,
    // two boxes side by side, showed the line between them.
    const QuadMesh quad;
    const std::vector<BakeMesh> meshes = {quad.mesh()};
    const std::vector<BakeMaterial> materials = {BakeMaterial{Vec3{0.7F, 0.7F, 0.7F}}};
    std::vector<BakeInstance> instances(3);
    instances[0].transform = place({1, 0, 0}, {0, 1, 0}, 2.0F, 2.0F, Vec3{-1.0F, 0.0F, 0.0F});
    instances[1].transform = place({1, 0, 0}, {0, 1, 0}, 2.0F, 2.0F, Vec3{1.0F, 0.0F, 0.0F});
    // A wall across the far end, for bounce; it meets the tiles at a hard edge, which is no seam.
    instances[2].transform = place({1, 0, 0}, {0, 0, 1}, 4.0F, 2.0F, Vec3{0.0F, 1.0F, -1.0F});
    std::vector<gi::GiLight> lights(1);
    lights[0].position = Vec3{0.5F, 1.5F, 0.3F};
    lights[0].intensity = 20.0F;
    lights[0].range = 20.0F;
    LightmapScene scene;
    scene.meshes = {meshes.data(), meshes.size()};
    scene.materials = {materials.data(), materials.size()};
    scene.instances = {instances.data(), instances.size()};
    scene.lights = {lights.data(), lights.size()};

    LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    settings.trace.samples = 8;
    settings.denoise = false;
    settings.atlas.texel_density = 6.0F;
    // Along x = 0: the left tile's u = 1 and the right tile's u = 0, at the same v.
    const auto disagreement = [&](const BakedLightmap& lightmap) {
        f32 worst = 0.0F;
        f32 mean = 0.0F;
        for (u32 step = 0; step <= 16; ++step) {
            const f32 v = static_cast<f32>(step) / 16.0F;
            const Vec3 left =
                sample_lightmap(lightmap, lightmap.addresses[0], Vec2{1.0F, v}, Vec3{0, 1, 0});
            const Vec3 right =
                sample_lightmap(lightmap, lightmap.addresses[1], Vec2{0.0F, v}, Vec3{0, 1, 0});
            worst = std::max(worst, std::fabs(luminance(left) - luminance(right)));
            mean += luminance(left);
        }
        return worst / (mean / 17.0F);
    };
    const Baked reconciled = bake(scene, settings);
    settings.reconcile_seams = false;
    const Baked raw = bake(scene, settings);
    const f32 before = disagreement(raw.lightmap);
    const f32 after = disagreement(reconciled.lightmap);
    CY_TEST_MESSAGE("seam between objects: " << reconciled.report.seam_edges << " edge(s), "
                                             << before << " unreconciled, " << after
                                             << " reconciled");
    CY_CHECK_EQ(reconciled.report.seam_edges, 1U);
    CY_CHECK_GT(before, 0.02F);
    CY_CHECK_LT(after, 0.01F);
}

// --- Emission, alpha tests and transparency ----------------------------------------------------

CY_TEST_CASE("an emissive surface lights the room with no placed light at all") {
    Room room;
    room.materials[kCeiling].emission = Vec3{2.0F, 2.0F, 2.0F};
    const LightmapScene scene = room.scene();
    LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    settings.atlas.texel_density = 3.0F;
    const Baked lit = bake(scene, settings);
    const Vec3 up{0.0F, 1.0F, 0.0F};
    const Vec2 centre{0.5F, 0.5F};
    const Vec3 floor = sample_lightmap(lit.lightmap, lit.lightmap.addresses[kFloor], centre, up);
    const std::vector<Vec3> truth = reference(
        scene, settings, {quad_point(room.instances[kFloor].transform, centre)}, {up}, 1024);
    CY_CHECK_GT(luminance(floor), 0.1F);
    CY_CHECK_NEAR(luminance(floor), luminance(truth[0]), 0.1F * luminance(truth[0]));

    // The control: the same room with the panel switched off is black. One sample is enough to be
    // exactly zero.
    room.materials[kCeiling].emission = Vec3{};
    LightmapBakeSettings control = settings;
    control.trace.samples = 1;
    const Baked dark = bake(room.scene(), control);
    CY_CHECK_EQ(
        luminance(sample_lightmap(dark.lightmap, dark.lightmap.addresses[kFloor], centre, up)),
        0.0F);
}

namespace {

/// An open floor under a noon sun with a panel held above it. Direct light only, black sky, so a
/// floor texel is either in the panel's shadow or it is not.
struct ShadowStage {
    QuadMesh quad;
    std::vector<BakeMesh> meshes;
    std::vector<BakeMaterial> materials;
    std::vector<BakeInstance> instances;
    std::vector<gi::GiLight> lights;
    std::vector<f32> mask_texels = {0.0F, 1.0F};  // left half cut out, right half solid
    AlphaMask mask{2, 1, {}};

    ShadowStage() {
        meshes.push_back(quad.mesh());
        materials.push_back(BakeMaterial{Vec3{0.5F, 0.5F, 0.5F}});
        materials.push_back(BakeMaterial{Vec3{0.5F, 0.5F, 0.5F}});
        instances.resize(2);
        instances[0].transform = place({1, 0, 0}, {0, 1, 0}, 4.0F, 4.0F, Vec3{});
        instances[1].material = 1;
        // The panel covers x in [-1, 1] at 1 m, facing down; it receives no lightmap of its own.
        instances[1].transform = place({1, 0, 0}, {0, -1, 0}, 2.0F, 4.0F, Vec3{0, 1, 0});
        instances[1].receives_lightmap = false;
        lights.resize(1);
        lights[0].directional = true;
        lights[0].direction = Vec3{0.0F, -1.0F, 0.0F};
        lights[0].intensity = 10.0F;
        mask.alpha = {mask_texels.data(), mask_texels.size()};
    }

    [[nodiscard]] LightmapScene scene() const {
        LightmapScene out;
        out.meshes = {meshes.data(), meshes.size()};
        out.materials = {materials.data(), materials.size()};
        out.instances = {instances.data(), instances.size()};
        out.lights = {lights.data(), lights.size()};
        out.sky.zenith = Vec3{};
        out.sky.horizon = Vec3{};
        out.sky.ground = Vec3{};
        return out;
    }

    [[nodiscard]] static LightmapBakeSettings settings() {
        LightmapBakeSettings out = small_settings(LightmapMode::Irradiance);
        out.content = LightmapContent::DirectAndIndirect;
        out.trace.bounces = 0;
        out.trace.samples = 4;
        out.denoise = false;
        return out;
    }

    /// The mean floor value over a band of world x, at a UV row through the middle.
    [[nodiscard]] static f32 band(const BakedLightmap& lightmap, f32 x_low, f32 x_high) {
        f32 total = 0.0F;
        u32 count = 0;
        for (u32 step = 0; step < 24; ++step) {
            for (u32 row = 0; row < 4; ++row) {
                const f32 x = x_low + ((x_high - x_low) * (static_cast<f32>(step) + 0.5F) / 24.0F);
                const Vec2 uv{(x / 4.0F) + 0.5F, 0.3F + (0.1F * static_cast<f32>(row))};
                total +=
                    luminance(sample_lightmap(lightmap, lightmap.addresses[0], uv, Vec3{0, 1, 0}));
                count += 1;
            }
        }
        return total / static_cast<f32>(count);
    }
};

}  // namespace

CY_TEST_CASE("an alpha-tested surface shadows through its coverage, a transparent one partly") {
    ShadowStage stage;
    const f32 sun = 10.0F / std::numbers::pi_v<f32>;  // E / pi under a 10 lux sun

    // Solid panel: the floor under it is dark, the floor beside it is lit.
    const Baked solid = bake(stage.scene(), ShadowStage::settings());
    CY_CHECK_NEAR(ShadowStage::band(solid.lightmap, 1.3F, 1.9F), sun, 0.02F * sun);
    CY_CHECK_LT(ShadowStage::band(solid.lightmap, -0.8F, -0.2F), 0.02F * sun);
    CY_CHECK_LT(ShadowStage::band(solid.lightmap, 0.2F, 0.8F), 0.02F * sun);

    // Alpha-tested: the panel's left half is cut out, so its shadow is the right half's alone.
    stage.materials[1].mask = &stage.mask;
    const Baked masked = bake(stage.scene(), ShadowStage::settings());
    CY_CHECK_NEAR(ShadowStage::band(masked.lightmap, -0.8F, -0.2F), sun, 0.02F * sun);
    CY_CHECK_LT(ShadowStage::band(masked.lightmap, 0.2F, 0.8F), 0.02F * sun);

    // Transparent at a quarter opacity: three quarters of the light through, on average.
    stage.materials[1].mask = nullptr;
    stage.materials[1].opacity = 0.25F;
    const Baked glass = bake(stage.scene(), ShadowStage::settings());
    CY_CHECK_NEAR(ShadowStage::band(glass.lightmap, -0.9F, 0.9F), 0.75F * sun, 0.12F * sun);
}

CY_TEST_CASE(
    "a texel buried in a neighbouring object is rejected and filled from its own surface") {
    // A closed crate resting on the floor: the floor texels under it see only the crate's inside.
    Room room(Vec3{2.0F, 1.25F, 2.0F});
    room.point_light(Vec3{0.0F, 0.9F, 0.0F}, 40.0F);
    const Vec3 x{1, 0, 0};
    const Vec3 y{0, 1, 0};
    const Vec3 z{0, 0, 1};
    const f32 h = 0.4F;
    const Vec3 c{0.6F, -1.25F + h, 0.6F};
    const Mat4 faces[6] = {
        place(x, -y, 2 * h, 2 * h, c - (y * h)), place(x, y, 2 * h, 2 * h, c + (y * h)),
        place(y, -x, 2 * h, 2 * h, c - (x * h)), place(y, x, 2 * h, 2 * h, c + (x * h)),
        place(x, -z, 2 * h, 2 * h, c - (z * h)), place(x, z, 2 * h, 2 * h, c + (z * h))};
    for (const Mat4& face : faces) {
        BakeInstance instance;
        instance.transform = face;
        instance.material = 0;
        instance.receives_lightmap = false;
        room.instances.push_back(instance);
    }
    // Few samples: what is measured is which texels are buried and what fills them, not how
    // converged the light is.
    LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    settings.trace.samples = 8;
    const Baked baked = bake(room.scene(), settings);
    CY_CHECK_GT(baked.report.texels_buried, 0U);
    // Under the crate's centre, the value is the floor's own neighbourhood rather than black.
    const Vec2 under{(c.x / 4.0F) + 0.5F, (c.z / 4.0F) + 0.5F};
    const Vec3 value =
        sample_lightmap(baked.lightmap, baked.lightmap.addresses[kFloor], under, Vec3{0, 1, 0});
    CY_CHECK_GT(luminance(value), 0.0F);
}

namespace {

/// Two charts in one unwrap: a 4 m floor (normal +y) in the left of the UV2 square and a wall
/// (normal +x) in the right, so the floor's right edge in the atlas borders the wall's padding.
struct FloorAndWallMesh {
    std::vector<Vec3> positions = {{-2.0F, 0.0F, -2.0F}, {-2.0F, 0.0F, 2.0F}, {2.0F, 0.0F, 2.0F},
                                   {2.0F, 0.0F, -2.0F},  {3.0F, 0.0F, -2.0F}, {3.0F, 4.0F, -2.0F},
                                   {3.0F, 4.0F, 2.0F},   {3.0F, 0.0F, 2.0F}};
    std::vector<Vec3> normals = {{0, 1, 0}, {0, 1, 0}, {0, 1, 0}, {0, 1, 0},
                                 {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}};
    std::vector<Vec2> uv2 = {{0.05F, 0.05F}, {0.05F, 0.95F}, {0.45F, 0.95F}, {0.45F, 0.05F},
                             {0.55F, 0.05F}, {0.95F, 0.05F}, {0.95F, 0.95F}, {0.55F, 0.95F}};
    std::vector<cy::u32> indices = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};

    [[nodiscard]] BakeMesh mesh() const {
        BakeMesh out;
        out.positions = {positions.data(), positions.size()};
        out.normals = {normals.data(), normals.size()};
        out.uv2 = {uv2.data(), uv2.size()};
        out.indices = {indices.data(), indices.size()};
        out.uv_coverage = 0.72F;
        return out;
    }
};

/// A closed box of outward quads that receives no lightmap: what buries the texels under it.
void add_box(std::vector<BakeInstance>& instances, Vec3 centre, Vec3 half) {
    const Vec3 x{1, 0, 0};
    const Vec3 y{0, 1, 0};
    const Vec3 z{0, 0, 1};
    const Mat4 faces[6] = {place(x, -y, 2 * half.x, 2 * half.z, centre - (y * half.y)),
                           place(x, y, 2 * half.x, 2 * half.z, centre + (y * half.y)),
                           place(y, -x, 2 * half.y, 2 * half.z, centre - (x * half.x)),
                           place(y, x, 2 * half.y, 2 * half.z, centre + (x * half.x)),
                           place(x, -z, 2 * half.x, 2 * half.y, centre - (z * half.z)),
                           place(x, z, 2 * half.x, 2 * half.y, centre + (z * half.z))};
    for (const Mat4& face : faces) {
        BakeInstance instance;
        instance.mesh = 1;
        instance.transform = face;
        instance.receives_lightmap = false;
        instances.push_back(instance);
    }
}

}  // namespace

CY_TEST_CASE("a wide buried strip is filled from its own chart, not the padding beside it") {
    // REGRESSION: the dilation ran a fixed four gutters' worth of passes and filled a texel from
    // every known neighbour in its rectangle, whichever chart it belonged to. A crate standing on
    // the floor's edge buries a strip of floor 26 texels wide that borders the wall chart's
    // padding: the strip's middle stayed black, and its edge took the WALL's light and normal, so
    // a directional read at the floor's own normal was no longer the floor's light.
    const FloorAndWallMesh level;
    const QuadMesh quad;
    const std::vector<BakeMesh> meshes = {level.mesh(), quad.mesh()};
    const std::vector<BakeMaterial> materials = {BakeMaterial{Vec3{0.7F, 0.7F, 0.7F}}};
    std::vector<BakeInstance> instances(1);
    add_box(instances, Vec3{1.3F, 0.5F, 0.0F}, Vec3{0.9F, 0.5F, 2.2F});
    std::vector<gi::GiLight> lights(1);
    lights[0].position = Vec3{-1.0F, 3.0F, 0.0F};
    lights[0].intensity = 30.0F;
    lights[0].range = 20.0F;
    LightmapScene scene;
    scene.meshes = {meshes.data(), meshes.size()};
    scene.materials = {materials.data(), materials.size()};
    scene.instances = {instances.data(), instances.size()};
    scene.lights = {lights.data(), lights.size()};
    scene.sky.zenith = Vec3{0.2F, 0.2F, 0.2F};
    scene.sky.horizon = scene.sky.zenith;
    scene.sky.ground = scene.sky.zenith;

    LightmapBakeSettings settings = small_settings(LightmapMode::Directional);
    settings.atlas.page_size = 256;
    // 16 texels a metre on the floor: the buried strip is 26 texels wide, filled from one side.
    settings.atlas.texel_density = 24.0F;
    settings.trace.bounces = 1;
    settings.trace.samples = 4;
    settings.denoise = false;
    const Baked baked = bake(scene, settings);
    CY_REQUIRE(baked.report.texels_buried > 200U);

    const BakedLightmap& lightmap = baked.lightmap;
    const u32 address = lightmap.addresses[0];
    u32 dark = 0;
    f32 worst = 0.0F;
    for (u32 row = 0; row <= 20; ++row) {
        const f32 v = 0.1F + (0.8F * static_cast<f32>(row) / 20.0F);
        // Across the buried strip: world x from 0.4 to 2, the floor's u from 0.29 to 0.45.
        for (u32 column = 0; column <= 16; ++column) {
            const Vec2 uv{0.29F + (0.16F * static_cast<f32>(column) / 16.0F), v};
            const Vec2 at =
                atlas_coordinate(address, uv, lightmap.page_size, lightmap.gutter_texels);
            dark += sample_plane(lightmap.texels, 0, at).y <= 0.0F ? 1U : 0U;
            // At the floor's own normal the directional factor is one, if what is read is floor.
            const Vec4 first = sample_plane(lightmap.texels, 0, at);
            const Vec3 read = sample_lightmap(lightmap, address, uv, Vec3{0.0F, 1.0F, 0.0F});
            worst = std::max(worst, std::fabs(read.y - first.y) / std::max(first.y, 1.0e-6F));
        }
    }
    CY_TEST_MESSAGE("buried strip: " << dark << " dark samples, worst directional factor error "
                                     << worst);
    CY_CHECK_EQ(dark, 0U);
    CY_CHECK_LT(worst, 0.01F);
}

// --- The seeds ---------------------------------------------------------------------------------

namespace {

/// The GI suites' room wired into an illumination system, as `render_gi_pipeline` builds it.
struct SeedRoom {
    gi_support::RoomField field{Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ}};
    std::vector<gi::Surfel> surfels = gi_support::room_surfels(1.0F, false);
    std::vector<gi::GiLight> lights = gi_support::room_lights();
    gi::IlluminationSystem system;

    SeedRoom() {
        CY_REQUIRE(system.configure(gi_support::room_settings()).has_value());
        CY_REQUIRE(system.field().place(1, field.asset(), cy::Mat4::identity()).has_value());
        const cy::Aabb bounds = cy::Aabb::from_center_extents(
            Vec3{},
            Vec3{gi_support::kRoomX + 1.0F, gi_support::kRoomY + 1.0F, gi_support::kRoomZ + 1.0F});
        CY_REQUIRE(
            system.scene().ingest_cell(1, bounds, {surfels.data(), surfels.size()}, 0).has_value());
        CY_REQUIRE(system.surfaces().allocate_from(system.scene(), bounds).has_value());
        system.surfaces().set_lookup_radius(1.2F);
    }

    [[nodiscard]] gi::FrameContext context(u64 frame) const {
        gi::FrameContext ctx;
        ctx.lights = {lights.data(), lights.size()};
        ctx.frame = frame;
        ctx.measured_gi_ms = 1.0F;
        return ctx;
    }
};

[[nodiscard]] u32 valid_probes(const gi::RadianceCache& cache) {
    u32 valid = 0;
    for (const gi::Probe& probe : cache.probes()) {
        valid += probe.live && probe.valid ? 1U : 0U;
    }
    return valid;
}

}  // namespace

CY_TEST_CASE("the lightmap bake seeds the dynamic caches from the same run") {
    SeedRoom seeded;
    // One frame places the probes; then they are invalidated, so what is measured is the bake's
    // seeding and not that frame's gather — the trap `render_gi_pipeline`'s seed case fell into.
    (void)seeded.system.update(seeded.context(0));
    const cy::Aabb everywhere = cy::Aabb::from_center_extents(Vec3{}, Vec3{1.0e4F, 1.0e4F, 1.0e4F});
    CY_REQUIRE(seeded.system.radiance().invalidate(everywhere) > 0U);
    CY_REQUIRE_EQ(valid_probes(seeded.system.radiance()), 0U);

    // The same room as lightmapped quads.
    Room room(Vec3{gi_support::kRoomX, gi_support::kRoomY, gi_support::kRoomZ});
    room.lights = seeded.lights;
    LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    settings.atlas.texel_density = 1.5F;
    settings.trace.samples = 8;
    CacheSeedTargets targets;
    targets.surfaces = &seeded.system.surfaces();
    targets.radiance = &seeded.system.radiance();
    targets.reflections = &seeded.system.reflection_probes();
    targets.settings.bounces = 1;
    targets.settings.samples = 8;
    targets.settings.max_distance_metres = 24.0F;
    const Baked baked = bake(room.scene(), settings, &targets);

    CY_CHECK_GT(baked.report.texels_covered, 0U);
    CY_CHECK_GT(baked.report.seeds.probes_seeded, 0U);
    CY_CHECK_GT(baked.report.seeds.surface_pages_seeded, 0U);
    CY_CHECK_GT(baked.report.seeds.rays, 0U);
    // One run's tracer counted both: the lightmap's rays and the seeds'.
    CY_CHECK_GT(baked.report.rays, baked.report.seeds.rays);
    CY_CHECK_EQ(valid_probes(seeded.system.radiance()), seeded.system.radiance().probe_count());

    // And a Hybrid level answers on its first frame from the seeds rather than from zero.
    seeded.system.set_mode(gi::GiMode::Hybrid);
    const gi::SurfaceProperties surface;
    const gi::ResolveResult first =
        seeded.system.indirect_diffuse(Vec3{-2.0F, -1.6F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F}, surface);
    CY_CHECK_GT(first.confidence, 0.0F);
    CY_CHECK_GT(cy::length(first.radiance), 0.0F);
}

// --- The cooked payload ------------------------------------------------------------------------

CY_TEST_CASE("the cooked lightmap round-trips, and an unchanged level bakes to the same bytes") {
    Room room;
    room.point_light(Vec3{0.0F, 0.8F, 0.0F}, 30.0F);
    LightmapBakeSettings settings = small_settings(LightmapMode::Directional);
    settings.trace.samples = 8;
    const Baked first = bake(room.scene(), settings);
    const Baked second = bake(room.scene(), settings);

    cy::Array<u8> a;
    cy::Array<u8> b;
    CY_REQUIRE(encode_lightmap_asset(first.lightmap, a).has_value());
    CY_REQUIRE(encode_lightmap_asset(second.lightmap, b).has_value());
    CY_REQUIRE_EQ(a.size(), b.size());
    CY_CHECK(std::equal(a.begin(), a.end(), b.begin()));
    CY_CHECK_EQ(first.lightmap.device_bytes(), u64{128} * 128 * first.lightmap.pages * 2 * 8);

    BakedLightmap decoded;
    CY_REQUIRE(decode_lightmap_asset({a.data(), a.size()}, decoded).has_value());
    CY_CHECK_EQ(decoded.mode, LightmapMode::Directional);
    CY_CHECK_EQ(decoded.addresses.size(), first.lightmap.addresses.size());
    for (usize index = 0; index < decoded.texels.texels.size(); index += 97) {
        const Vec4 x = decoded.texels.texels[index];
        const Vec4 y = first.lightmap.texels.texels[index];
        CY_CHECK_EQ(x.x, y.x);
        CY_CHECK_EQ(x.w, y.w);
    }
    // A truncated payload is refused rather than read past.
    CY_CHECK_FALSE(decode_lightmap_asset({a.data(), a.size() - 2}, decoded).has_value());
}

// --- Light mobility and the shadow mask (`add-lightmap-mobility-and-rebake`) -------------------

namespace {

/// The room with one point light near the ceiling and, below it, a small horizontal blocker facing
/// the floor: the floor straight under the lamp is in its shadow, and the floor at x = 1.2 sees the
/// lamp past the blocker's +x edge with half a metre to spare. The blocker owns a lightmap whose
/// every texel faces away from the lamp, so its whole chart — and its dilated gutter — is shadow.
struct ShadowRoom {
    Room room;
    Vec3 lamp{-0.6F, 1.0F, 0.0F};

    explicit ShadowRoom(gi::LightMobility mobility) {
        room.point_light(lamp, 30.0F);
        room.lights.back().mobility = mobility;
        BakeInstance blocker;
        blocker.mesh = 0;
        blocker.material = static_cast<u32>(room.materials.size());
        blocker.transform =
            place(Vec3{1, 0, 0}, Vec3{0, -1, 0}, 0.8F, 0.8F, Vec3{-0.8F, 0.3F, 0.0F});
        blocker.id = room.instances.size();
        room.instances.push_back(blocker);
        room.materials.push_back(BakeMaterial{Vec3{0.7F, 0.7F, 0.7F}});
    }

    /// A floor point: under the blocker, or out in the open on the +x side.
    [[nodiscard]] static Vec2 shadowed_uv() { return Vec2{0.35F, 0.5F}; }
    [[nodiscard]] static Vec2 lit_uv() { return Vec2{0.8F, 0.5F}; }
    [[nodiscard]] Vec3 floor_point(Vec2 uv) const {
        return quad_point(room.instances[kFloor].transform, uv);
    }
};

/// Whether atlas texel `index` lies in the rectangle `address` names.
[[nodiscard]] bool in_rectangle(const BakedLightmap& lightmap, u32 address, usize index) {
    AtlasPlacement placement;
    if (!decode_address(address, placement)) {
        return false;
    }
    const u32 block = lightmap.page_size / kAddressBlocks;
    const auto x = static_cast<u32>(index % lightmap.texels.width);
    const auto y = static_cast<u32>(index / lightmap.texels.width);
    const u32 top = (placement.page * lightmap.page_size) + (placement.block_y * block);
    const u32 left = placement.block_x * block;
    return x >= left && x < left + (placement.block_width * block) && y >= top &&
           y < top + (placement.block_height * block);
}

/// The blocker's instance index in `ShadowRoom`.
constexpr u32 kBlockerInstance = kFaces;

[[nodiscard]] f32 mask_at(const Baked& baked, u64 light, Vec2 uv, u32 instance = kFloor) {
    const Vec2 coordinate =
        atlas_coordinate(baked.lightmap.addresses[instance], uv, baked.lightmap.page_size,
                         baked.lightmap.gutter_texels);
    return sample_shadow_mask(baked.lightmap, light, coordinate);
}

[[nodiscard]] bool same_texels(const LightmapTexels& a, const LightmapTexels& b) {
    return a.width == b.width && a.height == b.height && a.planes == b.planes &&
           a.texels.size() == b.texels.size() &&
           std::equal(a.texels.begin(), a.texels.end(), b.texels.begin(), [](Vec4 p, Vec4 q) {
               return p.x == q.x && p.y == q.y && p.z == q.z && p.w == q.w;
           });
}

[[nodiscard]] f32 mean_luminance(const LightmapTexels& texels) {
    f64 total = 0.0;
    const usize plane = usize{texels.width} * texels.height;
    for (usize at = 0; at < plane; ++at) {
        const Vec4 texel = texels.texels[at];
        total += static_cast<f64>(luminance(Vec3{texel.x, texel.y, texel.z}));
    }
    return static_cast<f32>(total / static_cast<f64>(std::max<usize>(plane, 1)));
}

}  // namespace

CY_TEST_CASE("a stationary light bakes its indirect light and a shadow mask, not its direct term") {
    const ShadowRoom stationary(gi::LightMobility::Stationary);
    const LightmapScene scene = stationary.room.scene();
    const LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    const Baked baked = bake(scene, settings);
    const u64 light = stationary.room.lights.front().id;

    CY_REQUIRE_EQ(baked.lightmap.shadow_lights.size(), 1U);
    CY_CHECK_EQ(baked.lightmap.shadow_lights[0], light);
    const f32 in_shadow = mask_at(baked, light, ShadowRoom::shadowed_uv());
    const f32 in_light = mask_at(baked, light, ShadowRoom::lit_uv());
    CY_TEST_MESSAGE("shadow mask: " << in_shadow << " under the blocker, " << in_light
                                    << " in the open");
    CY_CHECK_LT(in_shadow, 0.1F);
    CY_CHECK_GT(in_light, 0.9F);
    // The mask is dilated with the texels: every texel of the blocker's rectangle — its chart, all
    // shadow, and the gutter the dilation fills from it — holds the chart's shadow, not the unlit
    // default a texel starts at. A coarser mip level averages exactly those gutter texels.
    const usize plane = usize{baked.lightmap.shadow_mask.width} * baked.lightmap.shadow_mask.height;
    u32 rectangle = 0;
    u32 lit_in_rectangle = 0;
    for (usize index = 0; index < plane; ++index) {
        if (in_rectangle(baked.lightmap, baked.lightmap.addresses[kBlockerInstance], index)) {
            rectangle += 1U;
            lit_in_rectangle += baked.lightmap.shadow_mask.texels[index].x > 0.05F ? 1U : 0U;
        }
    }
    CY_TEST_MESSAGE("blocker rectangle: " << lit_in_rectangle << " of " << rectangle
                                          << " texels unshadowed");
    CY_CHECK_GT(rectangle, 0U);
    CY_CHECK_EQ(lit_in_rectangle, 0U);

    // The texels are the indirect light only: they match the path tracer's own indirect-only
    // answer at the lit point, and a Static bake of the same room — which adds the direct term —
    // is far brighter there.
    const Vec3 normal{0.0F, 1.0F, 0.0F};
    const Vec2 lit = ShadowRoom::lit_uv();
    const Vec3 texel =
        sample_lightmap(baked.lightmap, baked.lightmap.addresses[kFloor], lit, normal);
    const std::vector<Vec3> truth =
        reference(scene, settings, {stationary.floor_point(lit)}, {normal}, 1024);
    CY_TEST_MESSAGE("stationary texel " << luminance(texel) << " against indirect-only truth "
                                        << luminance(truth[0]));
    CY_CHECK_LT(std::fabs(luminance(texel) - luminance(truth[0])), 0.25F * luminance(truth[0]));

    // A Static bake of the same room adds exactly the direct term: the difference between the two
    // texels is the light's shadowed direct term at the point, over pi (a texel holds E / pi).
    const ShadowRoom as_static(gi::LightMobility::Static);
    const Baked static_baked = bake(as_static.room.scene(), settings);
    const Vec3 static_texel = sample_lightmap(static_baked.lightmap,
                                              static_baked.lightmap.addresses[kFloor], lit, normal);
    MeshSceneTracer occluder;
    CY_REQUIRE(occluder.build(scene).has_value());
    const f32 direct =
        luminance(gi::shaded_direct(cy::Span<const gi::GiLight>(stationary.room.lights.data(), 1),
                                    stationary.floor_point(lit), normal, &occluder)) /
        std::numbers::pi_v<f32>;
    const f32 added = luminance(static_texel) - luminance(texel);
    CY_TEST_MESSAGE("static adds " << added << " over stationary; the direct term over pi is "
                                   << direct);
    CY_CHECK_GT(direct, 0.1F * luminance(texel));
    CY_CHECK_LT(std::fabs(added - direct), 0.15F * direct);

    // The runtime's half: the light's unshadowed direct term, at whatever intensity the frame gives
    // it, times the baked mask, is the path tracer's shadowed direct term — in the shadow and out
    // of it, and at three times the baked intensity with nothing re-baked.
    for (const f32 scale : {1.0F, 3.0F}) {
        gi::GiLight runtime = stationary.room.lights.front();
        runtime.intensity *= scale;
        const cy::Span<const gi::GiLight> one(&runtime, 1);
        MeshSceneTracer tracer;
        CY_REQUIRE(tracer.build(scene).has_value());
        for (const Vec2 uv : {ShadowRoom::shadowed_uv(), ShadowRoom::lit_uv()}) {
            const Vec3 point = stationary.floor_point(uv);
            const Vec3 open = gi::shaded_direct(one, point, normal, nullptr);
            const Vec3 shadowed = gi::shaded_direct(one, point, normal, &tracer);
            const Vec3 from_mask = open * mask_at(baked, light, uv);
            CY_CHECK_LE(std::fabs(luminance(from_mask) - luminance(shadowed)),
                        0.05F * std::max(luminance(open), 1.0e-4F));
        }
    }
}

CY_TEST_CASE("a movable light bakes nothing, not even its bounce") {
    const ShadowRoom movable(gi::LightMobility::Movable);
    const Baked baked = bake(movable.room.scene(), small_settings(LightmapMode::Irradiance));
    CY_CHECK(baked.lightmap.shadow_lights.empty());
    CY_CHECK(baked.lightmap.shadow_mask.texels.empty());
    // A closed room with a black sky and no emission: with its only light movable, nothing is left.
    const f32 mean = mean_luminance(baked.lightmap.texels);
    CY_TEST_MESSAGE("movable-only room: mean texel luminance " << mean);
    CY_CHECK_LT(mean, 1.0e-5F);

    const ShadowRoom stationary(gi::LightMobility::Stationary);
    const Baked lit = bake(stationary.room.scene(), small_settings(LightmapMode::Irradiance));
    CY_CHECK_GT(mean_luminance(lit.lightmap.texels), 100.0F * std::max(mean, 1.0e-7F));
}

CY_TEST_CASE("a static light bakes exactly what DirectAndIndirect baked before mobility existed") {
    const ShadowRoom as_static(gi::LightMobility::Static);
    const LightmapBakeSettings settings = small_settings(LightmapMode::Directional);
    const Baked static_baked = bake(as_static.room.scene(), settings);

    // The pre-mobility spelling: every light's direct term through the content switch.
    const ShadowRoom stationary(gi::LightMobility::Stationary);
    LightmapBakeSettings legacy = settings;
    legacy.content = LightmapContent::DirectAndIndirect;
    const Baked legacy_baked = bake(stationary.room.scene(), legacy);

    CY_CHECK(same_texels(static_baked.lightmap.texels, legacy_baked.lightmap.texels));
    // Neither writes a mask: the static light has no channel, and DirectAndIndirect bakes every
    // non-movable light's direct term, leaving nothing for a mask to shadow.
    CY_CHECK(static_baked.lightmap.shadow_lights.empty());
    CY_CHECK(legacy_baked.lightmap.shadow_lights.empty());
}

CY_TEST_CASE("a fifth stationary light is refused by name") {
    Room room;
    for (u32 index = 0; index < kMaxShadowMaskLights + 1U; ++index) {
        room.point_light(Vec3{-1.5F + (static_cast<f32>(index) * 0.7F), 1.0F, 0.0F}, 10.0F);
    }
    BakedLightmap out;
    LightmapBakeReport report;
    const cy::Status baked = bake_lightmaps(room.scene(), small_settings(LightmapMode::Irradiance),
                                            nullptr, out, report);
    CY_REQUIRE_FALSE(baked.has_value());
    CY_CHECK_EQ(baked.error().code, cy::ErrorCode::OutOfRange);

    room.lights.back().mobility = gi::LightMobility::Movable;
    CY_CHECK(
        bake_lightmaps(room.scene(), small_settings(LightmapMode::Irradiance), nullptr, out, report)
            .has_value());
    CY_CHECK_EQ(out.shadow_lights.size(), kMaxShadowMaskLights);
}

CY_TEST_CASE("the cooked lightmap carries the shadow mask, and a version 1 payload still decodes") {
    const ShadowRoom stationary(gi::LightMobility::Stationary);
    const Baked baked = bake(stationary.room.scene(), small_settings(LightmapMode::Irradiance));
    cy::Array<u8> payload;
    CY_REQUIRE(encode_lightmap_asset(baked.lightmap, payload).has_value());
    BakedLightmap decoded;
    CY_REQUIRE(decode_lightmap_asset(payload.span(), decoded).has_value());
    CY_CHECK(same_texels(decoded.texels, baked.lightmap.texels));
    CY_CHECK(same_texels(decoded.shadow_mask, baked.lightmap.shadow_mask));
    CY_REQUIRE_EQ(decoded.shadow_lights.size(), 1U);
    CY_CHECK_EQ(decoded.shadow_lights[0], baked.lightmap.shadow_lights[0]);

    // A version 1 payload is this one cut after the texels, with the old version word.
    const ShadowRoom movable(gi::LightMobility::Movable);
    const Baked plain = bake(movable.room.scene(), small_settings(LightmapMode::Irradiance));
    CY_REQUIRE(encode_lightmap_asset(plain.lightmap, payload).has_value());
    const usize texel_bytes = plain.lightmap.texels.texels.size() * 8U;
    const usize version_one_size = ((8U + plain.lightmap.addresses.size()) * 4U) + texel_bytes;
    CY_REQUIRE(payload.size() > version_one_size);
    cy::Array<u8> old;
    CY_REQUIRE(old.resize(version_one_size).has_value());
    std::memcpy(old.data(), payload.data(), old.size());
    const u8 version_one[4] = {1, 0, 0, 0};
    std::memcpy(old.data() + 4, version_one, sizeof(version_one));
    BakedLightmap from_old;
    CY_REQUIRE(decode_lightmap_asset(old.span(), from_old).has_value());
    CY_CHECK(same_texels(from_old.texels, plain.lightmap.texels));
    CY_CHECK(from_old.shadow_lights.empty());
}

// --- Incremental rebake -----------------------------------------------------------------------

namespace {

/// Four floor tiles ten metres apart under an open sky, and a small receiving blocker hovering over
/// the first. Moving the blocker reaches the first tile and nothing else.
struct TileLevel {
    QuadMesh quad;
    std::vector<BakeMesh> meshes;
    std::vector<BakeMaterial> materials{BakeMaterial{Vec3{0.6F, 0.6F, 0.6F}}};
    std::vector<BakeInstance> instances;
    static constexpr u32 kBlocker = 4;

    TileLevel() {
        meshes.push_back(quad.mesh());
        for (u32 tile = 0; tile < 4U; ++tile) {
            BakeInstance instance;
            instance.transform = place(Vec3{1, 0, 0}, Vec3{0, 1, 0}, 2.0F, 2.0F,
                                       Vec3{10.0F * static_cast<f32>(tile), 0.0F, 0.0F});
            instance.id = tile;
            instances.push_back(instance);
        }
        BakeInstance blocker;
        blocker.transform = blocker_at(0.0F);
        blocker.id = kBlocker;
        instances.push_back(blocker);
    }

    [[nodiscard]] static Mat4 blocker_at(f32 x) {
        return place(Vec3{1, 0, 0}, Vec3{0, 1, 0}, 0.8F, 0.8F, Vec3{x, 0.4F, 0.0F});
    }

    [[nodiscard]] LightmapScene scene() const {
        LightmapScene out;
        out.meshes = {meshes.data(), meshes.size()};
        out.materials = {materials.data(), materials.size()};
        out.instances = {instances.data(), instances.size()};
        return out;  // the default sky: an open level lit by it alone
    }
};

[[nodiscard]] LightmapBakeSettings tile_settings() {
    LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    settings.atlas.texel_density = 12.0F;
    return settings;
}

}  // namespace

CY_TEST_CASE("moving one object re-solves its region and keeps every other texel byte for byte") {
    TileLevel level;
    const LightmapBakeSettings settings = tile_settings();
    const Baked before = bake(level.scene(), settings);

    const Aabb old_bounds = Aabb::from_min_max(Vec3{-0.4F, 0.4F, -0.4F}, Vec3{0.4F, 0.4F, 0.4F});
    level.instances[TileLevel::kBlocker].transform = TileLevel::blocker_at(0.5F);
    const u32 moved = TileLevel::kBlocker;
    LightmapRebakeRequest request;
    request.moved_instances = {&moved, 1};
    request.previous_bounds = {&old_bounds, 1};
    request.influence_metres = 2.0F;

    BakedLightmap after;
    LightmapBakeReport report;
    CY_REQUIRE(rebake_lightmaps(level.scene(), settings, before.lightmap, request, after, report)
                   .has_value());
    CY_TEST_MESSAGE("rebake: incremental "
                    << report.incremental << ", objects " << report.objects_rebaked << " of "
                    << level.instances.size() << ", rays " << report.rays << " against "
                    << before.report.rays << " for the bake");
    CY_REQUIRE(report.incremental);
    CY_CHECK_EQ(report.objects_rebaked, 2U);  // the blocker and the tile under it
    CY_CHECK_LT(report.rays, before.report.rays);

    // Every texel that changed is inside the blocker's or the first tile's rectangle.
    const usize texels = usize{after.texels.width} * after.texels.height;
    u32 changed = 0;
    u32 changed_elsewhere = 0;
    for (usize index = 0; index < texels; ++index) {
        const Vec4 a = before.lightmap.texels.texels[index];
        const Vec4 b = after.texels.texels[index];
        if (a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w) {
            continue;
        }
        changed += 1U;
        if (!in_rectangle(after, after.addresses[0], index) &&
            !in_rectangle(after, after.addresses[TileLevel::kBlocker], index)) {
            changed_elsewhere += 1U;
        }
    }
    CY_CHECK_GT(changed, 0U);
    CY_CHECK_EQ(changed_elsewhere, 0U);

    // The re-solved region agrees with a full bake of the moved level.
    const Baked full = bake(level.scene(), settings);
    const Vec3 up{0.0F, 1.0F, 0.0F};
    for (const u32 object : {0U, TileLevel::kBlocker}) {
        for (const Vec2 uv : {Vec2{0.25F, 0.5F}, Vec2{0.5F, 0.5F}, Vec2{0.75F, 0.5F}}) {
            const f32 rebaked = luminance(sample_lightmap(after, after.addresses[object], uv, up));
            const f32 truth =
                luminance(sample_lightmap(full.lightmap, full.lightmap.addresses[object], uv, up));
            CY_CHECK_LE(std::fabs(rebaked - truth), 0.02F * std::max(truth, 1.0e-4F));
        }
    }
}

CY_TEST_CASE("a rebake with nothing moved traces nothing and changes nothing") {
    const TileLevel level;
    const LightmapBakeSettings settings = tile_settings();
    const Baked before = bake(level.scene(), settings);
    BakedLightmap after;
    LightmapBakeReport report;
    CY_REQUIRE(rebake_lightmaps(level.scene(), settings, before.lightmap, LightmapRebakeRequest{},
                                after, report)
                   .has_value());
    CY_CHECK(report.incremental);
    CY_CHECK_EQ(report.objects_rebaked, 0U);
    CY_CHECK_EQ(report.rays, 0U);
    CY_CHECK(same_texels(after.texels, before.lightmap.texels));
    CY_CHECK(std::ranges::equal(after.coverage, before.lightmap.coverage));
}

CY_TEST_CASE("a rebake whose level no longer packs the same falls back to a full bake") {
    TileLevel level;
    const LightmapBakeSettings settings = tile_settings();
    const Baked before = bake(level.scene(), settings);
    level.instances[2].resolution_scale = 3.0F;  // a bigger rectangle: every placement may move
    BakedLightmap after;
    LightmapBakeReport report;
    CY_REQUIRE(rebake_lightmaps(level.scene(), settings, before.lightmap, LightmapRebakeRequest{},
                                after, report)
                   .has_value());
    CY_CHECK_FALSE(report.incremental);
    CY_CHECK(std::string_view(report.fallback) ==
             "the level no longer packs to the same rectangles");
    const Baked full = bake(level.scene(), settings);
    CY_CHECK(same_texels(after.texels, full.lightmap.texels));
}

// --- Chart padding ----------------------------------------------------------------------------

CY_TEST_CASE("an unwrap's chart padding is checked against the rectangle the atlas gives it") {
    // One object of two charts 0.1 of its UV square apart. At a low density the rectangle squeezes
    // that gap below the two texels bilinear filtering needs; at a high one it survives.
    const SeamMesh seam;
    std::vector<BakeMesh> meshes{seam.mesh()};
    std::vector<BakeMaterial> materials{BakeMaterial{}};
    std::vector<BakeInstance> instances(1);
    LightmapScene scene;
    scene.meshes = {meshes.data(), meshes.size()};
    scene.materials = {materials.data(), materials.size()};
    scene.instances = {instances.data(), instances.size()};

    LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    settings.atlas.mip_levels = 0;
    settings.trace.samples = 4;

    settings.atlas.texel_density = 8.0F;
    const Baked squeezed = bake(scene, settings);
    CY_CHECK_EQ(squeezed.report.required_chart_gap, 2U);
    CY_REQUIRE_EQ(squeezed.report.padding_short.size(), 1U);
    CY_CHECK_EQ(squeezed.report.padding_short[0], 0U);

    settings.atlas.texel_density = 96.0F;
    const Baked roomy = bake(scene, settings);
    CY_CHECK(roomy.report.padding_short.empty());

    // And with the setting on, the squeezed level is refused rather than baked.
    settings.atlas.texel_density = 8.0F;
    settings.refuse_short_padding = true;
    BakedLightmap out;
    LightmapBakeReport report;
    CY_CHECK_FALSE(bake_lightmaps(scene, settings, nullptr, out, report).has_value());
}

// --- What the frame needs besides the texels (`add-lightmap-frame-shadow-mask`) -----------------

CY_TEST_CASE("the bake names every light whose direct term it baked, and no other") {
    const u64 light = 1;
    // Which lights are named does not depend on the texels' quality: the cheapest bake there is.
    LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    settings.trace.samples = 1;
    settings.trace.bounces = 1;
    settings.denoise = false;
    settings.reconcile_seams = false;
    const Baked as_static = bake(ShadowRoom(gi::LightMobility::Static).room.scene(), settings);
    CY_REQUIRE_EQ(as_static.lightmap.direct_lights.size(), 1U);
    CY_CHECK_EQ(as_static.lightmap.direct_lights[0], light);
    CY_CHECK(as_static.lightmap.shadow_lights.empty());

    const Baked stationary = bake(ShadowRoom(gi::LightMobility::Stationary).room.scene(), settings);
    CY_CHECK(stationary.lightmap.direct_lights.empty());
    CY_CHECK_EQ(stationary.lightmap.shadow_lights.size(), 1U);

    const Baked movable = bake(ShadowRoom(gi::LightMobility::Movable).room.scene(), settings);
    CY_CHECK(movable.lightmap.direct_lights.empty());

    // DirectAndIndirect bakes a stationary light's direct term too, so the frame must skip it.
    LightmapBakeSettings everything = settings;
    everything.content = LightmapContent::DirectAndIndirect;
    const Baked all = bake(ShadowRoom(gi::LightMobility::Stationary).room.scene(), everything);
    CY_REQUIRE_EQ(all.lightmap.direct_lights.size(), 1U);
    CY_CHECK_EQ(all.lightmap.direct_lights[0], light);
}

namespace {

/// The shadow room at a page size whose block protects one mip level.
[[nodiscard]] LightmapBakeSettings mip_settings() {
    LightmapBakeSettings settings = small_settings(LightmapMode::Directional);
    settings.atlas.page_size = 256;
    settings.atlas.texel_density = 8.0F;
    settings.trace.samples = 8;
    return settings;
}

[[nodiscard]] bool same_chain(const BakedLightmap& a, const BakedLightmap& b) {
    if (a.mip_levels != b.mip_levels) {
        return false;
    }
    for (u32 level = 1; level <= a.mip_levels; ++level) {
        if (!same_texels(lightmap_level(a, level), lightmap_level(b, level)) ||
            !same_texels(shadow_mask_level(a, level), shadow_mask_level(b, level))) {
            return false;
        }
    }
    return true;
}

}  // namespace

CY_TEST_CASE(
    "a bake fills the mip chain its padding protects, and the cooked lightmap carries it") {
    const ShadowRoom room(gi::LightMobility::Stationary);
    const LightmapBakeSettings settings = mip_settings();
    const Baked first = bake(room.room.scene(), settings);
    const Baked second = bake(room.room.scene(), settings);
    const BakedLightmap& lightmap = first.lightmap;
    CY_REQUIRE_EQ(lightmap.mip_levels, 1U);
    const LightmapTexels& level = lightmap_level(lightmap, 1);
    CY_CHECK_EQ(level.width, lightmap.texels.width / 2U);
    CY_CHECK_EQ(level.height, lightmap.texels.height / 2U);
    CY_CHECK_EQ(level.planes, 2U);
    CY_CHECK_EQ(shadow_mask_level(lightmap, 1).texels.size(), usize{level.width} * level.height);
    CY_CHECK(same_chain(first.lightmap, second.lightmap));
    // Every level-1 texel the floor's rectangle holds is lit by the floor, not the black between
    // rectangles: a coarse texel is only ever filtered from its own rectangle.
    AtlasPlacement floor;
    CY_REQUIRE(decode_address(lightmap.addresses[kFloor], floor));
    const u32 block = lightmap.page_size / kAddressBlocks / 2U;
    u32 dark = 0;
    for (u32 y = floor.block_y * block; y < (floor.block_y + floor.block_height) * block; ++y) {
        for (u32 x = floor.block_x * block; x < (floor.block_x + floor.block_width) * block; ++x) {
            dark += level.texels[level.index(0, x, y)].y <= 0.0F ? 1U : 0U;
        }
    }
    CY_CHECK_EQ(dark, 0U);

    cy::Array<u8> payload;
    CY_REQUIRE(encode_lightmap_asset(lightmap, payload).has_value());
    BakedLightmap decoded;
    CY_REQUIRE(decode_lightmap_asset(payload.span(), decoded).has_value());
    CY_CHECK(same_texels(decoded.texels, lightmap.texels));
    CY_CHECK(same_chain(decoded, lightmap));
    CY_CHECK_EQ(decoded.device_bytes(), lightmap.device_bytes());
    CY_CHECK_EQ(decoded.shadow_mask_bytes(), lightmap.shadow_mask_bytes());

    // A version 2 payload — no mip chain, no direct lights — still decodes, to the base alone.
    const usize version3_bytes = 4U + (lightmap.direct_lights.size() * 8U) + 4U +
                                 (level.texels.size() * 8U) +
                                 (shadow_mask_level(lightmap, 1).texels.size() * 8U);
    cy::Array<u8> old;
    CY_REQUIRE(old.resize(payload.size() - version3_bytes).has_value());
    std::memcpy(old.data(), payload.data(), old.size());
    const u8 version_two[4] = {2, 0, 0, 0};
    std::memcpy(old.data() + 4, version_two, sizeof(version_two));
    BakedLightmap from_old;
    CY_REQUIRE(decode_lightmap_asset(old.span(), from_old).has_value());
    CY_CHECK(same_texels(from_old.texels, lightmap.texels));
    CY_CHECK(same_texels(from_old.shadow_mask, lightmap.shadow_mask));
    CY_CHECK_EQ(from_old.mip_levels, 0U);
    // And a version 3 payload cut short in its chain is refused rather than read past.
    CY_CHECK_FALSE(
        decode_lightmap_asset({payload.data(), payload.size() - 8U}, decoded).has_value());
}

namespace {

struct ProgressLog {
    std::vector<LightmapBakeStage> stages;
    std::vector<u32> done;
    std::vector<u32> total;
    std::atomic<bool> cancel{false};
    /// Trace steps after which `cancel` is raised; zero never.
    u32 cancel_after = 0;
    u32 trace_steps = 0;

    static void record(void* user, LightmapBakeStage stage, u32 done, u32 total) noexcept {
        auto* log = static_cast<ProgressLog*>(user);
        log->stages.push_back(stage);
        log->done.push_back(done);
        log->total.push_back(total);
        if (stage == LightmapBakeStage::Trace && ++log->trace_steps == log->cancel_after) {
            log->cancel.store(true);
        }
    }

    [[nodiscard]] LightmapBakeProgress progress() {
        LightmapBakeProgress out;
        out.report = &ProgressLog::record;
        out.user = this;
        out.cancel = &cancel;
        return out;
    }
};

}  // namespace

CY_TEST_CASE("a bake reports its progress, and doing so changes none of its bytes") {
    const ShadowRoom room(gi::LightMobility::Stationary);
    const LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    const Baked quiet = bake(room.room.scene(), settings);
    ProgressLog log;
    const LightmapBakeProgress progress = log.progress();
    Baked reported;
    CY_REQUIRE(bake_lightmaps(room.room.scene(), settings, nullptr, reported.lightmap,
                              reported.report, &progress)
                   .has_value());
    cy::Array<u8> a;
    cy::Array<u8> b;
    CY_REQUIRE(encode_lightmap_asset(quiet.lightmap, a).has_value());
    CY_REQUIRE(encode_lightmap_asset(reported.lightmap, b).has_value());
    CY_CHECK(std::ranges::equal(a, b));

    CY_REQUIRE(log.stages.size() > 4U);
    CY_CHECK_EQ(log.stages.front(), LightmapBakeStage::Prepare);
    CY_CHECK_EQ(log.stages.back(), LightmapBakeStage::Finish);
    CY_CHECK_EQ(log.done.back(), log.total.back());
    // The stages arrive in order, and the trace counts up to every texel of the atlas.
    u32 last_trace = 0;
    u32 trace_total = 0;
    for (usize at = 0; at < log.stages.size(); ++at) {
        if (at > 0) {
            CY_CHECK_LE(static_cast<u32>(log.stages[at - 1]), static_cast<u32>(log.stages[at]));
        }
        if (log.stages[at] == LightmapBakeStage::Trace) {
            CY_CHECK_GE(log.done[at], last_trace);
            last_trace = log.done[at];
            trace_total = log.total[at];
        }
    }
    CY_CHECK_EQ(last_trace, trace_total);
    CY_CHECK_EQ(trace_total, quiet.lightmap.texels.width * quiet.lightmap.texels.height);
    CY_CHECK_GE(log.trace_steps, trace_total / kProgressTexels);
}

CY_TEST_CASE("a cancelled bake stops at its next step and says it was cancelled") {
    const ShadowRoom room(gi::LightMobility::Stationary);
    ProgressLog log;
    log.cancel_after = 3;
    const LightmapBakeProgress progress = log.progress();
    BakedLightmap out;
    LightmapBakeReport report;
    const cy::Status baked =
        bake_lightmaps(room.room.scene(), small_settings(LightmapMode::Irradiance), nullptr, out,
                       report, &progress);
    CY_REQUIRE_FALSE(baked.has_value());
    CY_CHECK_EQ(baked.error().code, cy::ErrorCode::Unavailable);
    CY_CHECK(report.cancelled);
    // Three trace steps ran and the fourth checkpoint stopped it: a few thousand texels, not the
    // atlas.
    CY_CHECK_EQ(log.trace_steps, 3U);
    CY_CHECK_LE(report.texels_covered + report.texels_buried, 3U * kProgressTexels);
    CY_CHECK(std::ranges::none_of(log.stages, [](LightmapBakeStage stage) {
        return stage == LightmapBakeStage::Filter || stage == LightmapBakeStage::Finish;
    }));
}
