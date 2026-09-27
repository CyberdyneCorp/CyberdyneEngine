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
#include <cy/test/test.h>

#include "support.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using namespace cy::rendering::lightmap_bake;  // NOLINT(google-build-using-namespace)
namespace gi = cy::rendering::gi;
using cy::f32;
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
    std::vector<Vec3> positions = {{-0.5F, 0.0F, -0.5F}, {-0.5F, 0.0F, 0.5F},
                                   {0.5F, 0.0F, 0.5F},   {0.5F, 0.0F, -0.5F}};
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
    std::vector<Vec3> positions = {// left half, x in [-0.5, 0]
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
    std::vector<Vec2> uv2 = {{0.05F, 0.05F}, {0.05F, 0.95F}, {0.45F, 0.95F}, {0.45F, 0.05F},
                             // mirrored: the seam edge (x = 0) is at u = 0.95 on this side
                             {0.95F, 0.05F}, {0.95F, 0.95F}, {0.55F, 0.95F}, {0.55F, 0.05F}};
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
    settings.surfel_spacing = 0.5F;
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
    const f32 radians = degrees * 3.14159265F / 180.0F;
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
        const Vec3 normal = cy::normalize(cy::transform_direction(instance.transform,
                                                                  Vec3{0.0F, 1.0F, 0.0F}));
        for (u32 row = 0; row < 3; ++row) {
            for (u32 column = 0; column < 4; ++column) {
                const Vec2 uv{0.15F + (0.23F * static_cast<f32>(column)),
                              0.2F + (0.3F * static_cast<f32>(row))};
                positions.push_back(quad_point(instance.transform, uv));
                normals.push_back(normal);
                measured.push_back(sample_lightmap(baked.lightmap,
                                                   baked.lightmap.addresses[face], uv, normal));
            }
        }
    }
    const std::vector<Vec3> truth = reference(scene, settings, positions, normals, 1024);
    const gi::ReferenceComparison comparison =
        gi::compare_against_reference({measured.data(), measured.size()},
                                      {truth.data(), truth.size()});
    CY_TEST_MESSAGE("lightmap against ground truth: relative " << comparison.relative_error
                                                              << ", max "
                                                              << comparison.max_absolute_error
                                                              << " of mean "
                                                              << comparison.mean_reference_magnitude);
    CY_CHECK_GT(comparison.mean_reference_magnitude, 0.01F);
    // THE STATED ERROR: a tenth of the mean, over twenty-four points at 32 samples per texel
    // denoised, against 1024 samples at the point.
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
    const std::vector<Vec3> truth = reference(scene, small_settings(LightmapMode::Irradiance),
                                              positions, normals, 2048);
    // The ground truth's own response: this scene has one for the modes to keep.
    CY_REQUIRE_GT(luminance(truth[1]), luminance(truth[0]) * 1.1F);
    CY_REQUIRE_GT(truth[1].x / truth[1].z, truth[0].x / truth[0].z);

    const auto read = [&](LightmapMode mode) {
        const Baked baked = bake(scene, small_settings(mode));
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
                                          << " reconciled");
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
            empty += reconciled.lightmap.coverage[(usize{y} * reconciled.lightmap.page_size) + x] ==
                             0U
                         ? 1U
                         : 0U;
        }
    }
    CY_CHECK_GT(empty, 0U);  // there is padding to fill...
    CY_CHECK_EQ(dark, 0U);   // ...and all of it is filled
    CY_CHECK_GT(reconciled.report.texels_dilated, 0U);
}

// --- Emission, alpha tests and transparency ----------------------------------------------------

CY_TEST_CASE("an emissive surface lights the room with no placed light at all") {
    Room room;
    room.materials[kCeiling].emission = Vec3{2.0F, 2.0F, 2.0F};
    const LightmapScene scene = room.scene();
    const LightmapBakeSettings settings = small_settings(LightmapMode::Irradiance);
    const Baked lit = bake(scene, settings);
    const Vec3 up{0.0F, 1.0F, 0.0F};
    const Vec2 centre{0.5F, 0.5F};
    const Vec3 floor = sample_lightmap(lit.lightmap, lit.lightmap.addresses[kFloor], centre, up);
    const std::vector<Vec3> truth =
        reference(scene, settings, {quad_point(room.instances[kFloor].transform, centre)}, {up},
                  1024);
    CY_CHECK_GT(luminance(floor), 0.1F);
    CY_CHECK_NEAR(luminance(floor), luminance(truth[0]), 0.1F * luminance(truth[0]));

    // The control: the same room with the panel switched off is black.
    room.materials[kCeiling].emission = Vec3{};
    const Baked dark = bake(room.scene(), settings);
    CY_CHECK_EQ(luminance(sample_lightmap(dark.lightmap, dark.lightmap.addresses[kFloor], centre,
                                          up)),
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
                total += luminance(
                    sample_lightmap(lightmap, lightmap.addresses[0], uv, Vec3{0, 1, 0}));
                count += 1;
            }
        }
        return total / static_cast<f32>(count);
    }
};

}  // namespace

CY_TEST_CASE("an alpha-tested surface shadows through its coverage, a transparent one partly") {
    ShadowStage stage;
    const f32 sun = 10.0F / 3.14159265F;  // E / pi under a 10 lux sun

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

CY_TEST_CASE("a texel buried in a neighbouring object is rejected and filled from its own surface") {
    // A closed crate resting on the floor: the floor texels under it see only the crate's inside.
    Room room(Vec3{2.0F, 1.25F, 2.0F});
    room.point_light(Vec3{0.0F, 0.9F, 0.0F}, 40.0F);
    const Vec3 x{1, 0, 0};
    const Vec3 y{0, 1, 0};
    const Vec3 z{0, 0, 1};
    const f32 h = 0.4F;
    const Vec3 c{0.6F, -1.25F + h, 0.6F};
    const Mat4 faces[6] = {place(x, -y, 2 * h, 2 * h, c - (y * h)), place(x, y, 2 * h, 2 * h, c + (y * h)),
                           place(y, -x, 2 * h, 2 * h, c - (x * h)), place(y, x, 2 * h, 2 * h, c + (x * h)),
                           place(x, -z, 2 * h, 2 * h, c - (z * h)), place(x, z, 2 * h, 2 * h, c + (z * h))};
    for (const Mat4& face : faces) {
        BakeInstance instance;
        instance.transform = face;
        instance.material = 0;
        instance.receives_lightmap = false;
        room.instances.push_back(instance);
    }
    const Baked baked = bake(room.scene(), small_settings(LightmapMode::Irradiance));
    CY_CHECK_GT(baked.report.texels_buried, 0U);
    // Under the crate's centre, the value is the floor's own neighbourhood rather than black.
    const Vec2 under{(c.x / 4.0F) + 0.5F, (c.z / 4.0F) + 0.5F};
    const Vec3 value = sample_lightmap(baked.lightmap, baked.lightmap.addresses[kFloor], under,
                                       Vec3{0, 1, 0});
    CY_CHECK_GT(luminance(value), 0.0F);
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
            Vec3{}, Vec3{gi_support::kRoomX + 1.0F, gi_support::kRoomY + 1.0F,
                         gi_support::kRoomZ + 1.0F});
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
    CY_REQUIRE_GT(seeded.system.radiance().invalidate(everywhere), 0U);
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
