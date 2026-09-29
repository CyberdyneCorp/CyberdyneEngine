// SPDX-License-Identifier: MIT
// A baked lightmap in the forward frame, on a Vulkan device with validation and synchronisation
// validation on. `render.lightmaps`.
//
// ================================================================================================
// THE SCENE
// ================================================================================================
//
// `render.light_probes`' corner, arranged through `pipeline_test::FrameScene`'s hooks exactly as
// that suite arranges it — the white floor slab, a red wall at x = -2.4, a white back wall at
// z = -9, two white cubes, one 100 000 lux sun travelling toward -x — so that with no lightmap the
// frame is the one `light_probes_absent.png` records. `references/lightmaps_absent.png` is a copy
// of that file (md5 7f782f6d…), drawn by the frame shader before the irradiance volume existed and
// long before the lightmap did.
//
// The lightmap is baked on the host by `lightmap_bake::bake_lightmaps` from the same boxes: the
// frame's own cube mesh, its six faces laid out in a 3 x 2 grid of UV2 cells, one instance per box,
// the same sun, and a uniform sky at the frame's own flat ambient. INDIRECT CONTENT: the frame
// shades the sun itself, so the lightmap holds the sky and the bounce and not the sun's direct
// term.
//
// ================================================================================================
// THE CASES
// ================================================================================================
//
//   (a) the back wall and the cube near the red wall are redder with the lightmap on, and the far
//       ones are not; with it off every one of them has the flat ambient's colour;
//   (b) the frame's term follows the host's `sample_lightmap` pixel for pixel;
//   (c) no lightmap bound is the committed pre-change reference, byte for byte, and so is a
//       lightmap bound that no draw addresses and one bound under a mode that excludes lightmaps;
//   (d) the irradiance volume and the lightmap together: a lightmapped surface takes the lightmap
//       alone and a surface without one takes the volume, as `gi::exclusion_for()` says;
//   (e) the directional and SH L1 encodings draw the irradiance encoding's picture at the
//       geometric normal, which is the only normal the frame has;
//   (f) an unchanged lightmap is not uploaded again;
//   (g)-(l) the baked lights, the mip chain and the density view, each case saying what it holds;
//   (m) a stationary sun through the darker of its mask channel and a bound real-time shadow map,
//       with a movable occluder only the map sees;
//   (n) the frame's time without the lightmap, with it and with its mask — a measurement.
//
// Every frame is the SECOND frame of its scene with the temporal history cut, as in
// `render.light_probes`, so the first frame — which the flat ambient is read from — never blends
// into what is measured.

#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#if defined(CY_TEST_LIGHTMAPS_METAL)
#    include <cy/backends/rhi-metal/backend.h>
#else
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/irradiance_volume.h>
#include <cy/rendering/gi/proxy_scene.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/light_probes/probe_volume_texture.h>
#include <cy/rendering/lightmap_bake/asset.h>
#include <cy/rendering/lightmap_bake/bake.h>
#include <cy/rendering/lightmap_bake/mips.h>
#include <cy/rendering/lightmaps/lightmap_textures.h>
#include <cy/test/test.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numbers>
#include <numeric>
#include <vector>

using namespace cy;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
namespace gi = cy::rendering::gi;
namespace bake = cy::rendering::lightmap_bake;
namespace lightmaps = cy::rendering::lightmaps;
namespace light_probes = cy::rendering::light_probes;
namespace pipeline = cy::rendering::pipeline;

namespace {

// --- The corner, as `render.light_probes` builds it ---------------------------------------------

constexpr u32 kLightmapSlot = 116;  // three consecutive slots: 116, 117, 118
constexpr u32 kVolumeSlot = 121;
constexpr u32 kShadowMaskSlot = 119;
constexpr u32 kShadowMapSlot = 120;
/// The level's texel density, texels per metre: the bake's, and the density view's target.
constexpr f32 kTexelDensity = 2.5F;
constexpr f32 kFloorTop = -1.9F + 0.125F;
constexpr f32 kBackWall = -9.0F;
constexpr f32 kRedWall = -2.4F;
constexpr Vec3 kSunTravel{-0.6F, -0.8F, 0.0F};
constexpr f32 kSunLux = 100000.0F;
constexpr f32 kExposureShift = -2.18F;
constexpr f32 kWhite = 0.8F;
constexpr Vec3 kRed{0.85F, 0.08F, 0.06F};

/// The real-time shadow map (m) binds: `render.volumetric_fog`'s orthographic box, 18 m across and
/// 2048 texels, 8.8 mm a texel, turned to this corner's sun.
constexpr f32 kShadowRadius = 9.0F;
constexpr f32 kShadowNear = 0.1F;
constexpr f32 kShadowFar = kShadowRadius * 4.0F;
constexpr Vec3 kShadowCentre{0.5F, -1.8F, -6.5F};
constexpr u32 kShadowExtent = 2048;
/// A movable box over the lit floor, in no bake: the occluder only the real-time map sees.
constexpr f32 kOccluderHalf = 0.35F;
constexpr Vec3 kOccluderCentre{1.4F, kFloorTop + 1.5F, -5.2F};

enum Box : u32 {
    kFloor = 0,
    kRedWallBox = 1,
    kBackLeft = 2,
    kBackRight = 3,
    kCubeNear = 4,
    kCubeFar = 5,
    kUsedBoxes = 6,
};

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

#if defined(CY_TEST_LIGHTMAPS_METAL)
constexpr const char* kBackendName = "metal";
constexpr rhi::BackendKind kBackend = rhi::BackendKind::Metal;
#else
constexpr const char* kBackendName = "vulkan";
constexpr rhi::BackendKind kBackend = rhi::BackendKind::Vulkan;
#endif

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "graphics validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
#if defined(CY_TEST_LIGHTMAPS_METAL)
        (void)rhi::metal::register_metal_backend();
#else
        (void)rhi::vulkan::register_vulkan_backend();
#endif
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_lightmaps";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        device_ = rhi::create_device(allocator_, kBackendName, description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &errors_);
        }
    }
    ~DeviceFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator_, device_.value());
        }
    }
    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;

    [[nodiscard]] bool has_gpu() const noexcept {
        return device_.has_value() && device_.value()->capabilities().backend() == kBackend;
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] u32 validation_errors() const noexcept { return errors_; }
    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "no requested graphics device on this machine; the backend selected was '%s' "
                     "because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

void save(const char* name, const std::vector<u32>& texels) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, Span<const u32>(texels.data(), texels.size()), kWidth, kHeight)
             .has_value()) {
        return;
    }
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    if (directory == nullptr || *directory == '\0') {
        directory = CY_TEST_BINARY_DIR;
    }
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", path, kWidth, kHeight);
    }
}

/// The rows of the stacked atlas some rectangle reaches: the picture is cropped to them, because a
/// level that uses the top fifth of its page would otherwise be a black square.
[[nodiscard]] u32 used_rows(const bake::BakedLightmap& lightmap) noexcept {
    const u32 block = lightmap.page_size / bake::kAddressBlocks;
    u32 rows = 1;
    for (const u32 address : lightmap.addresses) {
        bake::AtlasPlacement placement;
        if (bake::decode_address(address, placement)) {
            rows = std::max(rows, (placement.page * lightmap.page_size) +
                                      ((placement.block_y + placement.block_height) * block));
        }
    }
    return std::min(rows, lightmap.texels.height);
}

/// The first plane of a lightmap as a picture: the stacked texture cropped to its used rows,
/// exposed to its own mean and tonemapped. For the README, not for an assertion.
void save_atlas(const char* name, const bake::BakedLightmap& lightmap) noexcept {
    const bake::LightmapTexels& texels = lightmap.texels;
    const u32 rows = used_rows(lightmap);
    const usize count = usize{texels.width} * rows;
    f64 total = 0.0;
    for (usize index = 0; index < count; ++index) {
        total += static_cast<f64>(texels.texels[index].y);
    }
    const f64 mean = total / static_cast<f64>(count);
    const f64 scale = mean > 0.0 ? 0.5 / mean : 1.0;
    std::vector<u32> pixels(count);
    for (usize index = 0; index < pixels.size(); ++index) {
        const Vec4 texel = texels.texels[index];
        const auto encode = [scale](f32 texel_value) {
            const f64 value = static_cast<f64>(texel_value);
            const f64 mapped = (value * scale) / (1.0 + (value * scale));
            return static_cast<u32>(std::clamp(std::pow(mapped, 1.0 / 2.2), 0.0, 1.0) * 255.0);
        };
        pixels[index] =
            encode(texel.x) | (encode(texel.y) << 8U) | (encode(texel.z) << 16U) | 0xFF000000U;
    }
    render_test::Image image(allocator());
    if (!render_test::adopt(image, Span<const u32>(pixels.data(), pixels.size()), texels.width,
                            rows)
             .has_value()) {
        return;
    }
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    if (directory == nullptr || *directory == '\0') {
        directory = CY_TEST_BINARY_DIR;
    }
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", path, texels.width, rows);
    }
}

Vec3 albedo_of(u32 box) noexcept {
    return box == kRedWallBox ? kRed : Vec3{kWhite, kWhite, kWhite};
}

// --- The cube's lightmap coordinates ------------------------------------------------------------

/// `pipeline_test::CubeMesh`'s faces, in its order, and the tangent it builds each from.
constexpr Vec3 kFaceNormals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                                  Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
/// Each face's cell leaves this share of the cell on every side as chart padding. With the
/// resolution scales `bake_corner` gives, two cells are at least `required_chart_gap` — four
/// texels, two of the one mip level a 256 page protects — apart on every box, which is what keeps a
/// bilinear tap at a face's edge on its own face at every level: at 0.06 the small cubes' cells
/// were four texels wide with a quarter-texel of padding, and their edges read the next face; at
/// 0.1 five boxes were short of the mip level's gap, and the directional encoding's level-1 taps at
/// a far cube's edge read the next face's normal (two 8-bit steps in case (e)).
constexpr f32 kCellPadding = 0.18F;

[[nodiscard]] Vec3 face_tangent(Vec3 normal) noexcept {
    return std::fabs(normal.y) > 0.5F ? Vec3{1.0F, 0.0F, 0.0F} : Vec3{0.0F, 1.0F, 0.0F};
}

/// UV2 of a point on face `face`, at the face's own (u, v) in [-1, 1] — the corner parameters
/// `CubeMesh` places its vertices at.
[[nodiscard]] Vec2 cube_uv2(u32 face, f32 u, f32 v) noexcept {
    const auto column = static_cast<f32>(face % 3U);
    const auto row = static_cast<f32>(face >= 3U ? 1U : 0U);
    const f32 inner = 1.0F - (2.0F * kCellPadding);
    return Vec2{(column + kCellPadding + (((u * 0.5F) + 0.5F) * inner)) / 3.0F,
                (row + kCellPadding + (((v * 0.5F) + 0.5F) * inner)) / 2.0F};
}

/// The cube mesh again, with UV2: the positions and normals `CubeMesh` has, in its vertex order.
struct LightmappedCube {
    std::vector<Vec3> positions;
    std::vector<Vec3> normals;
    std::vector<Vec2> uv2;
    std::vector<u32> indices;

    LightmappedCube() {
        for (u32 face = 0; face < 6U; ++face) {
            const Vec3 normal = kFaceNormals[face];
            const Vec3 tangent = face_tangent(normal);
            const Vec3 bitangent = cross(normal, tangent);
            const auto first = static_cast<u32>(positions.size());
            for (u32 corner = 0; corner < 4U; ++corner) {
                const f32 u = (corner == 1U || corner == 2U) ? 1.0F : -1.0F;
                const f32 v = (corner >= 2U) ? 1.0F : -1.0F;
                positions.push_back((normal * 0.5F) + (tangent * (u * 0.5F)) +
                                    (bitangent * (v * 0.5F)));
                normals.push_back(normal);
                uv2.push_back(cube_uv2(face, u, v));
            }
            for (const u32 step : {0U, 1U, 2U, 0U, 2U, 3U}) {
                indices.push_back(first + step);
            }
        }
    }

    [[nodiscard]] bake::BakeMesh mesh() const {
        bake::BakeMesh out;
        out.positions = {positions.data(), positions.size()};
        out.normals = {normals.data(), normals.size()};
        out.uv2 = {uv2.data(), uv2.size()};
        out.indices = {indices.data(), indices.size()};
        out.uv_coverage = (1.0F - (2.0F * kCellPadding)) * (1.0F - (2.0F * kCellPadding));
        out.uv_aspect = 1.5F;
        return out;
    }
};

[[nodiscard]] Mat4 box_transform(const Aabb& box) noexcept {
    const Vec3 size = box.size();
    const Vec3 centre = box.center();
    return Mat4::from_columns(Vec4{size.x, 0, 0, 0}, Vec4{0, size.y, 0, 0}, Vec4{0, 0, size.z, 0},
                              Vec4{centre.x, centre.y, centre.z, 1.0F});
}

gi::GiLight gi_sun(gi::LightMobility mobility = gi::LightMobility::Stationary) noexcept {
    gi::GiLight light;
    light.mobility = mobility;
    light.directional = true;
    light.direction = normalize(kSunTravel);
    light.intensity = kSunLux;
    light.id = 1;
    return light;
}

/// A stationary lamp between the two cubes, low over the floor: they cast its shadows on the floor
/// away from it. Its stable id is 2, the frame's second light and the mask's second channel.
constexpr Vec3 kLampPosition{0.8F, kFloorTop + 1.3F, -6.5F};
constexpr f32 kLampCandela = 150000.0F;
constexpr f32 kLampRange = 14.0F;
constexpr u64 kLampId = 2;

gi::GiLight gi_lamp() noexcept {
    gi::GiLight light;
    light.position = kLampPosition;
    light.intensity = kLampCandela;
    light.range = kLampRange;
    light.mobility = gi::LightMobility::Stationary;
    light.id = kLampId;
    return light;
}

/// The corner baked once per encoding and kept for the whole run: a bake is the expensive thing
/// here, and every case bakes the same boxes under the same sky.
struct CornerBake {
    bake::BakedLightmap lightmap;
    bake::LightmapBakeReport report;
    f64 seconds = 0.0;
};

[[nodiscard]] const CornerBake* bake_corner(Span<const Aabb> boxes, Vec3 flat,
                                            bake::LightmapMode mode, gi::LightMobility mobility,
                                            bool lamp) {
    static std::unique_ptr<CornerBake> baked[12];
    const bool baked_direct = mobility == gi::LightMobility::Static;
    std::unique_ptr<CornerBake>& slot =
        baked[(static_cast<u32>(mode) * 4U) + (baked_direct ? 1U : 0U) + (lamp ? 2U : 0U)];
    if (slot != nullptr) {
        return slot.get();
    }
    static const LightmappedCube cube;
    const bake::BakeMesh mesh = cube.mesh();
    std::vector<bake::BakeMaterial> materials;
    std::vector<bake::BakeInstance> instances;
    for (u32 box = 0; box < kUsedBoxes && box < boxes.size(); ++box) {
        bake::BakeMaterial material;
        material.albedo = albedo_of(box);
        materials.push_back(material);
        bake::BakeInstance instance;
        instance.material = box;
        instance.transform = box_transform(boxes[box]);
        // The slab's six faces share one rectangle equally and its top is nearly half its area:
        // the scale gives the top the density the others get.
        // The small cubes get four times the density, so each face's cell is sixteen texels wide
        // rather than four and its padding more than a texel.
        instance.resolution_scale = 1.0F;
        if (box == kFloor) {
            instance.resolution_scale = 1.7F;
        } else if (box >= kCubeNear) {
            instance.resolution_scale = 4.0F;
        }
        instance.id = box;
        instances.push_back(instance);
    }
    const gi::GiLight lights[2] = {gi_sun(mobility), gi_lamp()};
    bake::LightmapScene scene;
    scene.meshes = {&mesh, 1};
    scene.materials = {materials.data(), materials.size()};
    scene.instances = {instances.data(), instances.size()};
    scene.lights = {lights, lamp ? 2U : 1U};
    scene.sky.zenith = flat;
    scene.sky.horizon = flat;
    scene.sky.ground = flat;

    bake::LightmapBakeSettings settings;
    settings.mode = mode;
    // One 256 page holds the corner; a 512 one was four fifths empty and every pass over the
    // atlas paid for the empty part.
    settings.atlas.page_size = 256;
    settings.atlas.texel_density = kTexelDensity;
    settings.trace.bounces = 1;
    settings.trace.samples = 24;
    settings.trace.max_distance_metres = 40.0F;
    settings.surfel_spacing = 0.5F;
    auto out = std::make_unique<CornerBake>();
    const auto started = std::chrono::steady_clock::now();
    const Status status =
        bake::bake_lightmaps(scene, settings, nullptr, out->lightmap, out->report);
    out->seconds = std::chrono::duration<f64>(std::chrono::steady_clock::now() - started).count();
    if (!status) {
        std::fprintf(stderr, "lightmaps: the corner bake failed: %s\n", status.error().message);
        return nullptr;
    }
    std::fprintf(stderr,
                 "lightmaps: %s bake of the corner in %.2f s: %u objects, %u page(s) of %u, %u "
                 "texels traced, %u dilated, %llu rays, %.1f KiB on the device; %zu object(s) "
                 "with charts closer than %u texels\n",
                 bake::lightmap_mode_name(mode), out->seconds, out->report.objects,
                 out->report.pages, out->lightmap.page_size, out->report.texels_covered,
                 out->report.texels_dilated, static_cast<unsigned long long>(out->report.rays),
                 static_cast<double>(out->lightmap.device_bytes()) / 1024.0,
                 out->report.padding_short.size(), out->report.required_chart_gap);
    // The mip chain's contract: every object's charts are as far apart as its levels need.
    CY_CHECK(out->report.padding_short.empty());
    slot = std::move(out);
    return slot.get();
}

/// The mip probe: a 1024 page whose one rectangle is the whole page and belongs to the floor, so
/// the floor's top face gets about twenty texels a metre and the far floor, seen at a grazing
/// angle, minifies through several levels. The base level is the flat ambient's grey and every
/// level below it red, so any pixel that reads a coarser level turns red — which it can only do if
/// the frame samples with the implicit level of detail and the upload carried the chain.
[[nodiscard]] std::unique_ptr<bake::BakedLightmap> mip_probe(Vec3 flat) {
    auto probe = std::make_unique<bake::BakedLightmap>();
    probe->mode = bake::LightmapMode::Irradiance;
    probe->page_size = 1024;
    probe->pages = 1;
    bake::AtlasSettings atlas;
    atlas.page_size = 1024;
    atlas.mip_levels = 3;
    probe->gutter_texels = bake::gutter_for(atlas);
    probe->mip_levels = 3;
    const f32 grey = (flat.x + flat.y + flat.z) / 3.0F;
    const auto fill = [](bake::LightmapTexels& texels, u32 size, Vec4 value) {
        texels.width = size;
        texels.height = size;
        texels.planes = 1;
        if (!texels.texels.resize(usize{size} * size).has_value()) {
            return false;
        }
        for (Vec4& texel : texels.texels) {
            texel = value;
        }
        return true;
    };
    if (!fill(probe->texels, 1024, Vec4{grey, grey, grey, 1.0F})) {
        return nullptr;
    }
    for (u32 level = 1; level <= probe->mip_levels; ++level) {
        if (!fill(probe->mip_texels[level - 1U], 1024U >> level,
                  Vec4{grey * 3.0F, 0.0F, 0.0F, 1.0F})) {
            return nullptr;
        }
    }
    const bake::AtlasPlacement whole{0, 0, 0, bake::kAddressBlocks, bake::kAddressBlocks, false};
    for (u32 box = 0; box < kUsedBoxes; ++box) {
        if (!probe->addresses
                 .push_back(box == kFloor ? bake::encode_address(whole) : bake::kNoLightmapAddress)
                 .has_value()) {
            return nullptr;
        }
    }
    return probe;
}

[[nodiscard]] bool copy_texels(const bake::LightmapTexels& from, bake::LightmapTexels& to) {
    to.width = from.width;
    to.height = from.height;
    to.planes = from.planes;
    to.texels.clear();
    return to.texels.append(from.texels.span()).has_value();
}

// --- The run ------------------------------------------------------------------------------------

enum class Lightmap : u8 {
    /// No lightmap anywhere: no stream, no textures, no words. The frame every caller that
    /// predates it draws.
    Absent,
    /// Stream, textures and words bound, and no draw addressed.
    Unaddressed,
    /// Everything bound and every draw addressed, under a mode that excludes lightmaps.
    ProbeMode,
    Enabled,
};

struct RunOptions {
    Lightmap lightmap = Lightmap::Absent;
    bake::LightmapMode mode = bake::LightmapMode::Irradiance;
    /// Draw the first plane of the directional bake as an irradiance lightmap.
    bool directional_as_irradiance = false;
    bool frame_sun = true;
    /// Attach the irradiance volume too.
    bool volume = false;
    /// Leave this box without a lightmap address.
    u32 unaddressed_box = ~0U;
    /// What the bake takes the sun to be: its direct term stays the frame's (`Stationary`, with a
    /// shadow-mask channel) or goes into the texels (`Static`).
    gi::LightMobility sun_mobility = gi::LightMobility::Stationary;
    /// The frame's sun intensity over the baked one: a stationary light moved at run time.
    f32 sun_scale = 1.0F;
    /// Tell the frame which of its lights the bake's sun is. Off, the frame's sun matches no baked
    /// light: it is shaded as the frame before the shadow mask shaded it, with no baked shadow.
    bool match_lights = true;
    /// Draw the texel-density view instead of the lit frame.
    bool density_view = false;
    /// Bake and shade the stationary lamp too, at this share of its baked intensity.
    bool lamp = false;
    f32 lamp_scale = 1.0F;
    /// Replace the bake by the mip probe: the floor alone, one flat colour at the base level and
    /// another at every level below it.
    bool mip_probe = false;
    /// Render the sun's real-time shadow map and bind it, as `render.volumetric_fog` does.
    bool shadow_map = false;
    /// Hang the movable occluder over the lit floor. It is in no bake and has no lightmap.
    bool occluder = false;
    /// The density view's target, texels per metre, and a shift of the frame's exposure in stops.
    f32 density_target = kTexelDensity;
    f32 exposure_shift = 0.0F;
};

struct Corner {
    FrameScene* scene = nullptr;
    RunOptions options;
    /// The frame's lights: the sun, then the lamp.
    render::LightDescription lights[2];
    std::vector<pipeline::InstanceTransform> instances;
    std::vector<u8> materials;
    std::vector<rendering::GpuDrawInstance> draws;
    const bake::BakedLightmap* lightmap = nullptr;
    lightmaps::LightmapTextures* textures = nullptr;
    light_probes::ProbeVolumeTexture* volume_texture = nullptr;
    const gi::IrradianceVolume* volume = nullptr;
    Vec3 flat{0.0F, 0.0F, 0.0F};
    bool attach = false;
    rhi::TextureHandle shadow_color;
    rhi::TextureHandle shadow_depth;
    rhi::TextureViewHandle shadow_view;
    rendering::FrameResourceRead shadow_read[1] = {};
};

/// The corner's boxes where `render.light_probes` puts them; and, for a case that asks, the movable
/// occluder in the first box past the baked ones.
void place_box(u32 which, Vec3& centre, f32& half, void* user) noexcept {
    const auto* corner = static_cast<const Corner*>(user);
    if (which == kUsedBoxes && corner != nullptr && corner->options.occluder) {
        half = kOccluderHalf;
        centre = kOccluderCentre;
        return;
    }
    switch (which) {
        case kRedWallBox:
            half = 3.0F;
            centre = Vec3{kRedWall - half, kFloorTop + half, -7.0F};
            return;
        case kBackLeft:
            half = 3.0F;
            centre = Vec3{-1.5F, kFloorTop + half, kBackWall - half};
            return;
        case kBackRight:
            half = 3.0F;
            centre = Vec3{4.5F, kFloorTop + half, kBackWall - half};
            return;
        case kCubeNear:
            half = 0.5F;
            centre = Vec3{-1.6F, kFloorTop + half, -6.5F};
            return;
        case kCubeFar:
            half = 0.5F;
            centre = Vec3{3.2F, kFloorTop + half, -6.5F};
            return;
        default:
            half = 0.3F;
            centre = Vec3{0.0F, -40.0F, 40.0F};
            return;
    }
}

void configure_corner(rendering::assembly::AssemblyDescription& description, void*) noexcept {
    description.pin_jitter = true;
    description.post.ambient_occlusion = false;
}

/// The sun's orthographic shadow volume over the corner, camera-relative — the camera is at the
/// origin — in the reversed-Z clip space the frame's `directionalShadowVisibility` reads.
[[nodiscard]] Mat4 shadow_to_clip() noexcept {
    const Vec3 travel = normalize(kSunTravel);
    const Vec3 eye = kShadowCentre - (travel * (kShadowRadius * 2.0F));
    const Mat4 view = look_at(eye, kShadowCentre, Vec3{0.0F, 0.0F, -1.0F});
    const Mat4 projection = orthographic_reversed_z(-kShadowRadius, kShadowRadius, -kShadowRadius,
                                                    kShadowRadius, kShadowNear, kShadowFar);
    return projection * view;
}

/// The frame's shadow pass into the corner's map, and the opaque pass's read of it — what
/// `render.volumetric_fog` declares.
void declare_shadow_map(Corner& corner, rendering::RenderGraph& graph,
                        rendering::assembly::AssemblyView& view,
                        rendering::assembly::FrameSinks& sinks) noexcept {
    rendering::TextureRequest request;
    request.name = "lightmaps test shadow map";
    request.format = rhi::Format::R32Sfloat;
    request.width = kShadowExtent;
    request.height = kShadowExtent;
    view.shadow_color =
        graph.import_texture(request, corner.shadow_color, rhi::ImageUse::Undefined);
    request.name = "lightmaps test shadow depth";
    request.format = rhi::Format::D32Sfloat;
    view.shadow_depth =
        graph.import_texture(request, corner.shadow_depth, rhi::ImageUse::Undefined);
    pipeline::FrameRecorder& recorder = corner.scene->recorder();
    recorder.set_shadow_targets(view.shadow_color, view.shadow_depth, kShadowExtent);
    const auto shadow = static_cast<usize>(rendering::FramePassKind::Shadow);
    sinks.passes[shadow] = recorder.sinks().passes[shadow];
    const auto opaque = static_cast<usize>(rendering::FramePassKind::Opaque);
    corner.shadow_read[0] =
        rendering::FrameResourceRead{view.shadow_color, rhi::Access::FragmentSampledRead};
    sinks.passes[opaque].reads = Span<const rendering::FrameResourceRead>(corner.shadow_read, 1);
}

Status before_assemble(rendering::RenderGraph& graph, rendering::assembly::AssemblyView& view,
                       rendering::assembly::FrameSinks& sinks, void* user) noexcept {
    auto* corner = static_cast<Corner*>(user);
    const u32 first = corner->options.frame_sun ? 0U : 1U;
    const u32 end = corner->options.lamp ? 2U : 1U;
    view.lights = Span<const render::LightDescription>(corner->lights + first, end - first);
    view.sun_direction = normalize(-kSunTravel);
    view.cut = true;
    if (corner->options.shadow_map) {
        declare_shadow_map(*corner, graph, view, sinks);
    }
    return ok();
}

/// The frame's words for the map: its slot, the sun — the frame's first light — and its extent.
void write_shadow_words(pipeline::FrameUpload& upload) noexcept {
    const Mat4 to_clip = shadow_to_clip();
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            upload.view.shadow_to_clip[(row * 4U) + column] = to_clip.at(row, column);
        }
    }
    upload.view.shadow_control[0] = kShadowMapSlot;
    upload.view.shadow_control[1] = 0U;
    upload.view.shadow_control[2] = kShadowExtent;
    upload.view.shadow_control[3] = 1U;
}

void whiten(Corner& corner, pipeline::FrameUpload& upload) noexcept {
    corner.materials.assign(upload.materials.begin(), upload.materials.end());
    const u32 stride = upload.view.counts[1];
    const usize blocks = corner.materials.size() / (static_cast<usize>(stride) * 4U);
    const auto write = [&](usize block, u32 word, f32 value) {
        std::memcpy(corner.materials.data() + (((block * stride) + word) * 4U), &value,
                    sizeof(value));
    };
    for (usize block = 0; block < blocks; ++block) {
        for (u32 channel = 0; channel < 3U; ++channel) {
            write(block, upload.view.material_offsets[0] + channel, 1.0F);
        }
        write(block, upload.view.material_offsets[1], 0.9F);
        write(block, upload.view.material_offsets[2], 0.0F);
    }
    upload.materials = Span<const u8>(corner.materials.data(), corner.materials.size());

    corner.instances.assign(upload.instances.begin(), upload.instances.end());
    for (u32 box = 0; box < corner.instances.size() && box < kUsedBoxes; ++box) {
        const Vec3 albedo = albedo_of(box);
        corner.instances[box].tint[0] = albedo.x;
        corner.instances[box].tint[1] = albedo.y;
        corner.instances[box].tint[2] = albedo.z;
    }
    upload.instances =
        Span<const pipeline::InstanceTransform>(corner.instances.data(), corner.instances.size());
}

/// Every draw of a lightmapped box carries its rectangle, as a surface query would have set it.
void address_draws(Corner& corner, pipeline::FrameUpload& upload) noexcept {
    corner.draws.assign(upload.draws.begin(), upload.draws.end());
    for (rendering::GpuDrawInstance& draw : corner.draws) {
        const u32 box = draw.instance_slot;
        if (box < kUsedBoxes && box != corner.options.unaddressed_box) {
            draw.gi_address = corner.lightmap->addresses[box];
        }
    }
    upload.draws = Span<const rendering::GpuDrawInstance>(corner.draws.data(), corner.draws.size());
}

/// The lightmap's planes, its shadow mask and — if asked for — the volume, bound to the frame's
/// set 0, and the slots the lightmap's planes and mask landed in.
[[nodiscard]] Status bind_frame_textures(Corner& corner,
                                         lightmaps::LightmapSlots& lightmap_slots) noexcept {
    pipeline::MaterialTextureSlot slots[lightmaps::kMaxPlanes + 3];
    u32 count = 0;
    if (corner.options.shadow_map) {
        slots[count++] = pipeline::MaterialTextureSlot{kShadowMapSlot, corner.shadow_view};
    }
    const u32 planes = corner.attach ? corner.textures->planes() : 0U;
    for (u32 plane = 0; plane < planes; ++plane) {
        lightmap_slots.planes[plane] = kLightmapSlot + plane;
        slots[count++] = corner.textures->slot(plane, kLightmapSlot + plane);
    }
    if (corner.attach && corner.textures->has_shadow_mask()) {
        lightmap_slots.shadow_mask = kShadowMaskSlot;
        slots[count++] = corner.textures->shadow_mask_slot(kShadowMaskSlot);
    }
    if (corner.attach && corner.options.volume) {
        slots[count++] = corner.volume_texture->slot(kVolumeSlot);
    }
    return corner.scene->set_frame_textures(
        Span<const pipeline::MaterialTextureSlot>(slots, count));
}

/// The lightmap's words, lights and draw addresses, for a corner whose lightmap is attached.
[[nodiscard]] Status write_corner_lightmap(Corner& corner, const lightmaps::LightmapSlots& slots,
                                           pipeline::FrameUpload& upload) noexcept {
    if (corner.options.volume) {
        light_probes::write_probe_volume(kVolumeSlot, *corner.volume,
                                         corner.volume_texture->layout(), Vec3{}, upload.view);
    }
    const gi::GiMode mode =
        corner.options.lightmap == Lightmap::ProbeMode ? gi::GiMode::Probe : gi::GiMode::Baked;
    // The stable ids of the frame's lights in its own order — the ids the bake gave them, or,
    // unmatched, ids no baked light has.
    u64 frame_lights[2] = {};
    u32 frame_count = 0;
    for (u32 light = corner.options.frame_sun ? 0U : 1U; light < (corner.options.lamp ? 2U : 1U);
         ++light) {
        frame_lights[frame_count++] =
            corner.options.match_lights ? corner.lights[light].stable_id : 900U + light;
    }
    if (Status written = lightmaps::write_lightmaps(
            slots, *corner.lightmap, mode, Span<const u64>(frame_lights, frame_count), upload.view);
        !written) {
        return written;
    }
    if (corner.options.density_view &&
        !lightmaps::write_lightmap_debug_view(render::DebugViewMode::LightmapDensity,
                                              corner.options.density_target, upload.view)) {
        return fail(ErrorCode::Internal, "the density view wrote nothing");
    }
    if (corner.options.lightmap != Lightmap::Unaddressed) {
        address_draws(corner, upload);
    }
    return ok();
}

Status before_upload(pipeline::FrameUpload& upload, void* user) noexcept {
    auto* corner = static_cast<Corner*>(user);
    whiten(*corner, upload);
    upload.globals.exposure_stops += kExposureShift + corner->options.exposure_shift;
    corner->flat = Vec3{upload.view.ambient_and_occlusion[0], upload.view.ambient_and_occlusion[1],
                        upload.view.ambient_and_occlusion[2]};
    if (corner->options.shadow_map) {
        write_shadow_words(upload);
    }
    if (!corner->attach && !corner->options.shadow_map) {
        return ok();
    }
    lightmaps::LightmapSlots lightmap_slots;
    if (Status bound = bind_frame_textures(*corner, lightmap_slots); !bound) {
        return bound;
    }
    return corner->attach ? write_corner_lightmap(*corner, lightmap_slots, upload) : ok();
}

/// The directional bake cut to its first plane — every level of it — with the same mask, lights
/// and addresses: what `(e)` compares the directional encoding against.
[[nodiscard]] std::unique_ptr<bake::BakedLightmap> first_plane_of(
    const bake::BakedLightmap& source) {
    auto out = std::make_unique<bake::BakedLightmap>();
    out->mode = bake::LightmapMode::Irradiance;
    out->page_size = source.page_size;
    out->pages = source.pages;
    out->gutter_texels = source.gutter_texels;
    out->texels.width = source.texels.width;
    out->texels.height = source.texels.height;
    out->texels.planes = 1;
    const usize count = usize{out->texels.width} * out->texels.height;
    if (!out->texels.texels.append(Span<const Vec4>(source.texels.texels.data(), count))
             .has_value() ||
        !out->addresses.append(source.addresses.span()).has_value()) {
        return nullptr;
    }
    // The same shadow mask, every level, and the same lights: the case compares the
    // encodings, so everything else is the directional bake's own.
    if (!copy_texels(source.shadow_mask, out->shadow_mask) ||
        !out->shadow_lights.append(source.shadow_lights.span()).has_value() ||
        !out->direct_lights.append(source.direct_lights.span()).has_value()) {
        return nullptr;
    }
    for (u32 level = 0; level < source.mip_levels; ++level) {
        if (!copy_texels(source.mip_shadow_mask[level], out->mip_shadow_mask[level])) {
            return nullptr;
        }
    }
    // And the first plane of every level of the chain, which the frame minifies into.
    out->mip_levels = source.mip_levels;
    for (u32 level = 0; level < source.mip_levels; ++level) {
        const bake::LightmapTexels& mip = source.mip_texels[level];
        bake::LightmapTexels& plane = out->mip_texels[level];
        plane.width = mip.width;
        plane.height = mip.height;
        plane.planes = 1;
        if (!plane.texels.append(Span<const Vec4>(mip.texels.data(), usize{mip.width} * mip.height))
                 .has_value()) {
            return nullptr;
        }
    }
    return out;
}

/// One corner, the lightmap baked from its boxes, and the frame measured.
class CornerRun {
public:
    CornerRun(DeviceFixture& fixture, RunOptions options)
        : device_(&fixture.device()), scene_(allocator()) {
        corner_.scene = &scene_;
        corner_.options = options;
        corner_.textures = &textures_;
        corner_.volume_texture = &volume_texture_;
        corner_.volume = &volume_;
        render::LightDescription& sun = corner_.lights[0];
        sun.kind = render::LightKind::Directional;
        sun.intensity = kSunLux * options.sun_scale;
        sun.transform.rotation = Quat::look_rotation(normalize(kSunTravel), Vec3{0.0F, 1.0F, 0.0F});
        sun.stable_id = 1;
        render::LightDescription& lamp = corner_.lights[1];
        lamp.kind = render::LightKind::Point;
        // The frame takes a point light in lumens (`lighting::default_unit_for`), the bake in
        // candela: the same lamp, 4 pi apart.
        lamp.intensity = kLampCandela * 4.0F * std::numbers::pi_v<f32> * options.lamp_scale;
        lamp.range = kLampRange;
        lamp.transform.translation = kLampPosition;
        lamp.stable_id = kLampId;
        FrameSceneHooks hooks;
        hooks.user = &corner_;
        hooks.configure = &configure_corner;
        hooks.before_assemble = &before_assemble;
        hooks.before_upload = &before_upload;
        hooks.place_box = &place_box;
        scene_.set_hooks(hooks);
        const Status built = scene_.build(fixture.device());
        if (!built) {
            std::fprintf(stderr, "lightmaps: build failed: %s\n", built.error().message);
        }
        ready_ = built.has_value() && (!options.shadow_map || create_shadow_map());
        scene_.set_read_back(true);
        textures_.initialize(fixture.device(), allocator());
        volume_texture_.initialize(fixture.device(), allocator());
    }

    ~CornerRun() {
        (void)device_->wait_idle();
        if (!uv_buffer_.is_null()) {
            device_->destroy_buffer(uv_buffer_);
        }
        if (!corner_.shadow_view.is_null()) {
            device_->destroy_texture_view(corner_.shadow_view);
        }
        for (const rhi::TextureHandle texture : {corner_.shadow_color, corner_.shadow_depth}) {
            if (!texture.is_null()) {
                device_->destroy_texture(texture);
            }
        }
    }

    CornerRun(const CornerRun&) = delete;
    CornerRun& operator=(const CornerRun&) = delete;

    [[nodiscard]] bool render() {
        if (!ready_) {
            return false;
        }
        rendering::assembly::AssemblyReport report{};
        if (!render_once(report)) {
            return false;
        }
        if (corner_.options.lightmap != Lightmap::Absent && !attach()) {
            return false;
        }
        if (!render_once(report_)) {
            return false;
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        return true;
    }

    /// One more frame of the scene `render` drew, with no readback, from the start of its
    /// recording to the device going idle, on the host's clock.
    [[nodiscard]] bool time_frame(f64& nanoseconds) {
        scene_.set_read_back(false);
        const auto started = std::chrono::steady_clock::now();
        if (!render_once(report_) || !device_->wait_idle().has_value()) {
            return false;
        }
        nanoseconds =
            std::chrono::duration<f64, std::nano>(std::chrono::steady_clock::now() - started)
                .count();
        return true;
    }

    [[nodiscard]] const std::vector<u32>& pixels() const { return pixels_; }
    [[nodiscard]] const rendering::assembly::AssemblyReport& report() const { return report_; }
    [[nodiscard]] FrameScene& scene() { return scene_; }
    [[nodiscard]] const bake::BakedLightmap& lightmap() const { return *corner_.lightmap; }
    [[nodiscard]] Vec3 flat() const { return corner_.flat; }
    [[nodiscard]] lightmaps::LightmapTextures& textures() { return textures_; }

private:
    [[nodiscard]] bool render_once(rendering::assembly::AssemblyReport& report) {
        const Status rendered = scene_.render(RecordMode::Callbacks, report);
        if (!rendered) {
            std::fprintf(stderr, "lightmaps: render failed: %s\n", rendered.error().message);
        }
        return rendered.has_value();
    }

    /// The sun's map — a colour target the frame samples and its depth — as
    /// `render.volumetric_fog` creates it.
    [[nodiscard]] bool create_shadow_map() {
        rhi::TextureDescription texture;
        texture.name = "lightmaps test shadow map";
        texture.format = rhi::Format::R32Sfloat;
        texture.extent = rhi::Extent3D{kShadowExtent, kShadowExtent, 1};
        texture.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        Expected<rhi::TextureHandle, Error> color = device_->create_texture(texture);
        if (!color.has_value()) {
            return false;
        }
        corner_.shadow_color = *color;
        texture.name = "lightmaps test shadow depth";
        texture.format = rhi::Format::D32Sfloat;
        texture.usage = rhi::TextureUsage::DepthStencilAttachment;
        Expected<rhi::TextureHandle, Error> depth = device_->create_texture(texture);
        if (!depth.has_value()) {
            return false;
        }
        corner_.shadow_depth = *depth;
        rhi::TextureViewDescription view;
        view.name = "lightmaps test shadow map";
        view.texture = corner_.shadow_color;
        Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
        if (!made.has_value()) {
            return false;
        }
        corner_.shadow_view = *made;
        return true;
    }

    /// The bake, the planes, the fourth stream and — if asked for — the volume.
    [[nodiscard]] bool attach() {
        const bake::LightmapMode mode = corner_.options.directional_as_irradiance
                                            ? bake::LightmapMode::Directional
                                            : corner_.options.mode;
        const CornerBake* baked = bake_corner(scene_.boxes(), corner_.flat, mode,
                                              corner_.options.sun_mobility, corner_.options.lamp);
        if (baked == nullptr) {
            return false;
        }
        corner_.lightmap = &baked->lightmap;
        if (corner_.options.mip_probe) {
            probe_ = mip_probe(corner_.flat);
            if (probe_ == nullptr) {
                return false;
            }
            corner_.lightmap = probe_.get();
        }
        if (corner_.options.directional_as_irradiance) {
            first_plane_ = first_plane_of(baked->lightmap);
            if (first_plane_ == nullptr) {
                return false;
            }
            corner_.lightmap = first_plane_.get();
        }
        if (!textures_.upload(*corner_.lightmap).has_value()) {
            return false;
        }
        if (corner_.options.volume && !capture_volume()) {
            return false;
        }
        static const LightmappedCube cube;
        rhi::BufferDescription description;
        description.name = "cube lightmap uvs";
        description.size = cube.uv2.size() * sizeof(Vec2);
        description.usage = rhi::BufferUsage::Vertex;
        description.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(description);
        if (!buffer.has_value()) {
            return false;
        }
        uv_buffer_ = *buffer;
        void* mapped = device_->buffer_mapped_pointer(uv_buffer_);
        if (mapped == nullptr) {
            return false;
        }
        std::memcpy(mapped, cube.uv2.data(), description.size);
        pipeline::GeometrySource geometry = scene_.recorder().geometry();
        geometry.lightmap_uvs = uv_buffer_;
        scene_.recorder().set_geometry(geometry);
        corner_.attach = true;
        return true;
    }

    [[nodiscard]] bool capture_volume() {
        const Span<const Aabb> boxes = scene_.boxes();
        proxies_.clear();
        for (u32 box = 0; box < kUsedBoxes && box < boxes.size(); ++box) {
            if (!proxies_.add(gi::ProxyBox{boxes[box], albedo_of(box), Vec3{}}).has_value()) {
                return false;
            }
        }
        light_ = gi_sun(corner_.options.sun_mobility);
        proxies_.set_lights(Span<const gi::GiLight>(&light_, 1));
        gi::IrradianceVolumeSettings settings;
        settings.origin = Vec3{-2.05F, kFloorTop + 0.3F, -8.7F};
        settings.spacing_metres = 0.75F;
        settings.count_x = 14;
        settings.count_y = 8;
        settings.count_z = 9;
        if (!volume_.configure(settings).has_value()) {
            return false;
        }
        gi::VolumeCaptureContext context;
        context.tracer = &proxies_;
        context.radiance = &proxies_;
        context.sky.zenith = corner_.flat;
        context.sky.horizon = corner_.flat;
        context.sky.ground = corner_.flat;
        (void)volume_.capture_all(context);
        proxies_.set_indirect(&volume_);
        (void)volume_.capture_all(context);
        return volume_texture_.upload(volume_).has_value();
    }

    rhi::Device* device_ = nullptr;
    FrameScene scene_;
    Corner corner_;
    lightmaps::LightmapTextures textures_;
    light_probes::ProbeVolumeTexture volume_texture_;
    gi::IrradianceVolume volume_;
    gi::BoxProxyScene proxies_;
    gi::GiLight light_{};
    std::unique_ptr<bake::BakedLightmap> first_plane_;
    std::unique_ptr<bake::BakedLightmap> probe_;
    rhi::BufferHandle uv_buffer_;
    rendering::assembly::AssemblyReport report_{};
    std::vector<u32> pixels_;
    bool ready_ = false;
};

// --- Reading the picture ------------------------------------------------------------------------

[[nodiscard]] Vec3 pixel_ray(const Mat4& projection, u32 x, u32 y) noexcept {
    const f32 ndc_x = (((static_cast<f32>(x) + 0.5F) / kWidth) * 2.0F) - 1.0F;
    const f32 ndc_y = 1.0F - (((static_cast<f32>(y) + 0.5F) / kHeight) * 2.0F);
    return Vec3{ndc_x / projection.columns[0].x, ndc_y / projection.columns[1].y, -1.0F};
}

[[nodiscard]] f32 ray_box(Vec3 ray, const Aabb& box) noexcept {
    f32 near = 0.0F;
    f32 far = INFINITY;
    const f32 direction[3] = {ray.x, ray.y, ray.z};
    const f32 low[3] = {box.min.x, box.min.y, box.min.z};
    const f32 high[3] = {box.max.x, box.max.y, box.max.z};
    for (u32 axis = 0; axis < 3; ++axis) {
        if (std::fabs(direction[axis]) < 1.0e-9F) {
            if (0.0F < low[axis] || 0.0F > high[axis]) {
                return INFINITY;
            }
            continue;
        }
        const f32 t0 = low[axis] / direction[axis];
        const f32 t1 = high[axis] / direction[axis];
        near = std::max(near, std::min(t0, t1));
        far = std::min(far, std::max(t0, t1));
        if (near > far) {
            return INFINITY;
        }
    }
    return near;
}

enum class Surface : u8 { None, Floor, BackWall, RedWall, CubeFront, Other };

struct Pixel {
    Surface surface = Surface::None;
    u32 box = 0;
    Vec3 point{0.0F, 0.0F, 0.0F};
};

[[nodiscard]] Pixel classify(Span<const Aabb> boxes, const Mat4& projection, u32 x, u32 y) {
    const Vec3 ray = pixel_ray(projection, x, y);
    Pixel pixel;
    f32 nearest = INFINITY;
    for (u32 box = 0; box < boxes.size(); ++box) {
        const f32 t = ray_box(ray, boxes[box]);
        if (t < nearest) {
            nearest = t;
            pixel.box = box;
        }
    }
    if (nearest == INFINITY) {
        return pixel;
    }
    pixel.point = ray * nearest;
    const Aabb& hit = boxes[pixel.box];
    const auto on = [](f32 a, f32 b) { return std::fabs(a - b) < 1.0e-3F; };
    if (pixel.box == kFloor && on(pixel.point.y, hit.max.y)) {
        pixel.surface = Surface::Floor;
    } else if ((pixel.box == kBackLeft || pixel.box == kBackRight) &&
               on(pixel.point.z, hit.max.z)) {
        pixel.surface = Surface::BackWall;
    } else if (pixel.box == kRedWallBox && on(pixel.point.x, hit.max.x)) {
        pixel.surface = Surface::RedWall;
    } else if ((pixel.box == kCubeNear || pixel.box == kCubeFar) && on(pixel.point.z, hit.max.z)) {
        pixel.surface = Surface::CubeFront;
    } else {
        pixel.surface = Surface::Other;
    }
    return pixel;
}

/// The face of `box` that `point` lies on, as `kFaceNormals`' index.
[[nodiscard]] u32 face_of(const Aabb& box, Vec3 point) noexcept {
    const f32 distances[6] = {std::fabs(point.x - box.max.x), std::fabs(point.x - box.min.x),
                              std::fabs(point.y - box.max.y), std::fabs(point.y - box.min.y),
                              std::fabs(point.z - box.max.z), std::fabs(point.z - box.min.z)};
    u32 nearest = 0;
    for (u32 face = 1; face < 6U; ++face) {
        nearest = distances[face] < distances[nearest] ? face : nearest;
    }
    return nearest;
}

/// UV2 of a point on a box: the face's own (u, v), from the point in the cube's model space.
[[nodiscard]] Vec2 uv2_of(const Aabb& box, Vec3 point, u32 face) noexcept {
    const Vec3 size = box.size();
    const Vec3 local = cwise_div(point - box.center(), size);
    const Vec3 normal = kFaceNormals[face];
    const Vec3 tangent = face_tangent(normal);
    const Vec3 bitangent = cross(normal, tangent);
    return cube_uv2(face, std::clamp(dot(local, tangent) * 2.0F, -1.0F, 1.0F),
                    std::clamp(dot(local, bitangent) * 2.0F, -1.0F, 1.0F));
}

struct Picture {
    std::vector<Pixel> pixels;
    std::vector<bool> interior;
};

[[nodiscard]] Picture classify_all(FrameScene& scene) {
    Picture picture;
    picture.pixels.resize(static_cast<usize>(kWidth) * kHeight);
    picture.interior.assign(picture.pixels.size(), false);
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            picture.pixels[(static_cast<usize>(y) * kWidth) + x] =
                classify(scene.boxes(), scene.projection(), x, y);
        }
    }
    for (u32 y = 1; y + 1 < kHeight; ++y) {
        for (u32 x = 1; x + 1 < kWidth; ++x) {
            const Pixel& centre = picture.pixels[(static_cast<usize>(y) * kWidth) + x];
            bool same = true;
            for (u32 dy = 0; dy < 3U && same; ++dy) {
                for (u32 dx = 0; dx < 3U && same; ++dx) {
                    const Pixel& other =
                        picture.pixels[(static_cast<usize>(y + dy - 1U) * kWidth) + (x + dx - 1U)];
                    same = other.surface == centre.surface && other.box == centre.box;
                }
            }
            picture.interior[(static_cast<usize>(y) * kWidth) + x] = same;
        }
    }
    return picture;
}

[[nodiscard]] i32 channel(u32 texel, u32 which) noexcept {
    return static_cast<i32>((texel >> (which * 8U)) & 0xFFU);
}

struct Tally {
    f64 red = 0.0;
    f64 green = 0.0;
    f64 blue = 0.0;
    u32 count = 0;

    void add(u32 texel) noexcept {
        red += channel(texel, 0);
        green += channel(texel, 1);
        blue += channel(texel, 2);
        ++count;
    }
    [[nodiscard]] f64 redness() const noexcept { return green > 0.0 ? red / green : 0.0; }
};

struct Bleed {
    Tally wall_near;
    Tally wall_far;
    Tally cube_near;
    Tally cube_far;
};

[[nodiscard]] Bleed measure_bleed(const Picture& picture, const std::vector<u32>& pixels) {
    Bleed bleed;
    for (usize index = 0; index < pixels.size(); ++index) {
        if (!picture.interior[index]) {
            continue;
        }
        const Pixel& pixel = picture.pixels[index];
        if (pixel.surface == Surface::BackWall && pixel.point.x < kRedWall + 1.0F) {
            bleed.wall_near.add(pixels[index]);
        } else if (pixel.surface == Surface::BackWall && pixel.point.x > 2.5F) {
            bleed.wall_far.add(pixels[index]);
        } else if (pixel.surface == Surface::CubeFront && pixel.box == kCubeNear) {
            bleed.cube_near.add(pixels[index]);
        } else if (pixel.surface == Surface::CubeFront && pixel.box == kCubeFar) {
            bleed.cube_far.add(pixels[index]);
        }
    }
    return bleed;
}

f64 correlation(const std::vector<f64>& a, const std::vector<f64>& b) {
    const auto n = static_cast<f64>(a.size());
    f64 mean_a = 0.0;
    f64 mean_b = 0.0;
    for (usize index = 0; index < a.size(); ++index) {
        mean_a += a[index] / n;
        mean_b += b[index] / n;
    }
    f64 covariance = 0.0;
    f64 variance_a = 0.0;
    f64 variance_b = 0.0;
    for (usize index = 0; index < a.size(); ++index) {
        covariance += (a[index] - mean_a) * (b[index] - mean_b);
        variance_a += (a[index] - mean_a) * (a[index] - mean_a);
        variance_b += (b[index] - mean_b) * (b[index] - mean_b);
    }
    return variance_a > 0.0 && variance_b > 0.0 ? covariance / std::sqrt(variance_a * variance_b)
                                                : 0.0;
}

[[nodiscard]] usize differing(const std::vector<u32>& a, const std::vector<u32>& b) noexcept {
    usize count = 0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        count += static_cast<usize>(a[index] != b[index]);
    }
    return count + (a.size() > b.size() ? a.size() - b.size() : b.size() - a.size());
}

[[nodiscard]] i32 worst_channel_delta(const std::vector<u32>& a, const std::vector<u32>& b,
                                      const std::vector<bool>* mask = nullptr) noexcept {
    i32 worst = 0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        if (mask != nullptr && !(*mask)[index]) {
            continue;
        }
        for (u32 which = 0; which < 3U; ++which) {
            worst = std::max(worst, std::abs(channel(a[index], which) - channel(b[index], which)));
        }
    }
    return worst;
}

// --- The committed reference --------------------------------------------------------------------

/// `references/lightmaps_absent.png`: the corner with no lightmap and no volume, drawn by the frame
/// shader of 0f1dfd1 — a byte-for-byte copy of `render.light_probes`' reference, whose corner this
/// suite rebuilds exactly.
///
/// ONE REFERENCE PER BACKEND: Metal's rasteriser and its compiled MSL round differently from
/// Vulkan's, so the two frames are not the same bytes (62 956 of 129 600 pixels differ, by a step
/// or two). `references/lightmaps_absent_metal.png` is the same corner drawn on the M2 Max by
/// main's frame shaders at 1b7373a5, before the shadow mask reached the frame: the Metal leg holds
/// the frame to it exactly as the Vulkan leg holds it to the other.
#if defined(CY_TEST_LIGHTMAPS_METAL)
constexpr const char* kBeforeReference = "lightmaps_absent_metal.png";
#else
constexpr const char* kBeforeReference = "lightmaps_absent.png";
#endif

const char* before_reference_path() noexcept {
    static char storage[1024];
    (void)std::snprintf(storage, sizeof(storage), "%s/references/%s", CY_LIGHTMAPS_TEST_DIR,
                        kBeforeReference);
    return storage;
}

void check_against_before(const std::vector<u32>& pixels) {
    render_test::Image reference(allocator());
    const Status read = render_test::read_png(before_reference_path(), reference);
    if (!read) {
        std::fprintf(stderr, "(c) %s: %s\n", before_reference_path(), read.error().message);
    }
    CY_REQUIRE(read.has_value());
    const std::vector<u32> committed(reference.texels.data(),
                                     reference.texels.data() + reference.texels.size());
    const usize changed = differing(committed, pixels);
    std::fprintf(stderr,
                 "(c) against the frame before lightmaps existed, byte for byte: %zu differ\n",
                 changed);
    CY_CHECK_EQ(changed, usize{0});
}

// --- The baked lights ---------------------------------------------------------------------------

/// The baked sun's shadow-mask value at a surface point, read off the host at one mip level: the
/// reference the frame's mask read is held to. The sun is channel 0, the lamp channel 1.
[[nodiscard]] f32 host_mask(const bake::BakedLightmap& lightmap, Span<const Aabb> boxes,
                            const Pixel& pixel, u32 level, u32 channel_index = 0) noexcept {
    const Aabb& box = boxes[pixel.box];
    const u32 face = face_of(box, pixel.point);
    const Vec2 coordinate =
        bake::atlas_coordinate(lightmap.addresses[pixel.box], uv2_of(box, pixel.point, face),
                               lightmap.page_size, lightmap.gutter_texels);
    const f32 scale = 1.0F / static_cast<f32>(1U << level);
    const Vec4 mask =
        bake::sample_plane(bake::shadow_mask_level(lightmap, level), 0, coordinate * scale);
    const f32 channels[4] = {mask.x, mask.y, mask.z, mask.w};
    return channels[channel_index & 3U];
}

[[nodiscard]] i32 brightness(u32 texel) noexcept {
    return channel(texel, 0) + channel(texel, 1) + channel(texel, 2);
}

/// The atlas coordinate a surface pixel reads, in base texels.
[[nodiscard]] Vec2 atlas_at(const bake::BakedLightmap& lightmap, Span<const Aabb> boxes,
                            const Pixel& pixel) noexcept {
    const Aabb& box = boxes[pixel.box];
    const u32 face = face_of(box, pixel.point);
    return bake::atlas_coordinate(lightmap.addresses[pixel.box], uv2_of(box, pixel.point, face),
                                  lightmap.page_size, lightmap.gutter_texels);
}

/// The deepest mip level the frame's implicit level of detail can blend in at an interior pixel:
/// from how far the atlas coordinate moves to the next pixel across and down, as the hardware's
/// derivatives measure it.
[[nodiscard]] u32 deepest_level(const Picture& picture, const bake::BakedLightmap& lightmap,
                                Span<const Aabb> boxes, usize index) noexcept {
    const Vec2 here = atlas_at(lightmap, boxes, picture.pixels[index]);
    const Vec2 across = atlas_at(lightmap, boxes, picture.pixels[index + 1U]) - here;
    const Vec2 down = atlas_at(lightmap, boxes, picture.pixels[index + kWidth]) - here;
    const f32 footprint = std::max(length(across), length(down));
    // Below a quarter of a level short of level 1, trilinear filtering reads level 0 alone.
    return footprint < 1.6F
               ? 0U
               : std::min(lightmap.mip_levels, 1U + static_cast<u32>(std::log2(footprint)));
}

/// The floor's interior pixels the baked sun sees none of, and all of.
struct FloorShadow {
    std::vector<usize> shadowed;
    std::vector<usize> lit;
};

enum class MaskState : u8 { Neither, Shadowed, Lit };

/// Fully shadowed and fully lit texels are exact in the mask (`lightmap_bake/README.md`), so a
/// bilinear read of four of them is exactly zero or exactly one — at every level the frame's
/// implicit level of detail may blend in at this pixel.
[[nodiscard]] MaskState mask_state(const Picture& picture, const bake::BakedLightmap& lightmap,
                                   Span<const Aabb> boxes, usize index, u32 channel) noexcept {
    const Pixel& pixel = picture.pixels[index];
    if (!picture.interior[index] || pixel.surface != Surface::Floor) {
        return MaskState::Neither;
    }
    bool shadowed = true;
    bool lit = true;
    const u32 deepest = deepest_level(picture, lightmap, boxes, index);
    for (u32 level = 0; level <= deepest; ++level) {
        const f32 mask = host_mask(lightmap, boxes, pixel, level, channel);
        shadowed = shadowed && mask == 0.0F;
        lit = lit && mask == 1.0F;
    }
    if (shadowed) {
        return MaskState::Shadowed;
    }
    return lit ? MaskState::Lit : MaskState::Neither;
}

/// Whether the pixel at (x, y), not on the image's edge, is shadowed or lit and so are its eight
/// neighbours, alike.
[[nodiscard]] bool uniform_around(const std::vector<MaskState>& states, u32 x, u32 y) noexcept {
    const MaskState centre = states[(usize{y} * kWidth) + x];
    if (centre == MaskState::Neither) {
        return false;
    }
    for (u32 dy = 0; dy < 3U; ++dy) {
        for (u32 dx = 0; dx < 3U; ++dx) {
            if (states[(usize{y + dy - 1U} * kWidth) + (x + dx - 1U)] != centre) {
                return false;
            }
        }
    }
    return true;
}

/// The floor pixels deep in the baked shadow and deep in the light: every pixel of their 3 x 3
/// neighbourhood reads an exact mask of the same value at its centre. The neighbourhood is the
/// margin for where the frame's pinned jitter actually samples the pixel, half a pixel away.
[[nodiscard]] FloorShadow floor_shadow(const Picture& picture, const bake::BakedLightmap& lightmap,
                                       Span<const Aabb> boxes, u32 channel = 0) {
    std::vector<MaskState> states(picture.pixels.size(), MaskState::Neither);
    for (usize index = 0; index < picture.pixels.size(); ++index) {
        states[index] = mask_state(picture, lightmap, boxes, index, channel);
    }
    FloorShadow out;
    for (u32 y = 1; y + 1U < kHeight; ++y) {
        for (u32 x = 1; x + 1U < kWidth; ++x) {
            const usize index = (usize{y} * kWidth) + x;
            if (uniform_around(states, x, y)) {
                (states[index] == MaskState::Shadowed ? out.shadowed : out.lit).push_back(index);
            }
        }
    }
    return out;
}

/// The mean brightness change from `a` to `b` over `pixels`, and the worst channel change.
struct Change {
    f64 mean = 0.0;
    i32 worst = 0;
};

[[nodiscard]] Change change_over(const std::vector<u32>& a, const std::vector<u32>& b,
                                 const std::vector<usize>& pixels) noexcept {
    Change out;
    for (const usize index : pixels) {
        out.mean += static_cast<f64>(brightness(b[index]) - brightness(a[index]));
        for (u32 which = 0; which < 3U; ++which) {
            out.worst =
                std::max(out.worst, std::abs(channel(a[index], which) - channel(b[index], which)));
        }
    }
    out.mean /= static_cast<f64>(std::max<usize>(pixels.size(), 1));
    return out;
}

// --- Reading a level back ----------------------------------------------------------------------

struct LevelReadback {
    rhi::TextureHandle texture;
    rhi::BufferHandle buffer;
    u32 level = 0;
    u32 width = 0;
    u32 height = 0;
};

void record_level_readback(const rendering::PassContext& context, void* user) noexcept {
    const auto* readback = static_cast<const LevelReadback*>(user);
    rhi::BufferTextureCopy region;
    region.mip_level = static_cast<u16>(readback->level);
    region.texture_extent = rhi::Extent3D{readback->width, readback->height, 1};
    context.commands->copy_texture_to_buffer(readback->texture, readback->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// One level of one of the lightmap's textures, as half floats, off the device.
[[nodiscard]] bool read_level(rhi::Device& device, const lightmaps::LightmapTextures& textures,
                              u32 plane, u32 width, u32 height, u32 level, std::vector<u16>& out) {
    LevelReadback readback;
    readback.texture = textures.texture(plane);
    readback.level = level;
    readback.width = width >> level;
    readback.height = height >> level;
    const u64 bytes = u64{readback.width} * readback.height * 4U * sizeof(u16);
    rhi::BufferDescription description;
    description.name = "lightmap level readback";
    description.size = bytes;
    description.usage = rhi::BufferUsage::TransferDestination;
    description.memory = rhi::MemoryUse::Readback;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(description);
    if (!buffer.has_value()) {
        return false;
    }
    readback.buffer = *buffer;
    rendering::RenderGraph graph(allocator());
    rendering::TextureRequest request;
    request.name = "lightmap level";
    request.format = rhi::Format::Rgba16Sfloat;
    request.width = width;
    request.height = height;
    request.mip_levels = static_cast<u16>(textures.mip_levels());
    const rendering::ResourceId image =
        graph.import_texture(request, readback.texture, rhi::ImageUse::SampledRead);
    rendering::BufferRequest destination_request;
    destination_request.name = "lightmap level readback";
    destination_request.size = bytes;
    destination_request.extra_usage = rhi::BufferUsage::TransferDestination;
    const rendering::ResourceId destination =
        graph.import_buffer(destination_request, readback.buffer);
    graph.add_pass("lightmap level readback", rhi::QueueKind::Graphics)
        .read(image, rhi::Access::TransferRead)
        .write(destination, rhi::Access::TransferWrite)
        .record(&record_level_readback, &readback);
    graph.add_pass("lightmap level host", rhi::QueueKind::Graphics)
        .read(destination, rhi::Access::HostRead)
        .side_effect();
    bool read = device.begin_frame().has_value();
    if (read) {
        rendering::GraphExecutor executor(allocator(), device);
        read = executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{})
                   .has_value() &&
               device.wait_idle().has_value();
        executor.release();
        read = device.end_frame().has_value() && read;
    }
    const auto* mapped = static_cast<const u16*>(device.buffer_mapped_pointer(readback.buffer));
    if (read && mapped != nullptr) {
        out.assign(mapped, mapped + (bytes / sizeof(u16)));
    }
    device.destroy_buffer(readback.buffer);
    return read && mapped != nullptr;
}

[[nodiscard]] std::vector<u16> host_halves(Span<const Vec4> texels) {
    std::vector<u16> out;
    out.reserve(texels.size() * 4U);
    for (const Vec4& texel : texels) {
        for (const f32 value : {texel.x, texel.y, texel.z, texel.w}) {
            out.push_back(bake::half_from_float(value));
        }
    }
    return out;
}

/// How a frame lit through both shadows compares with the two frames lit through one each.
struct DarkerOf {
    /// Pixels where a channel of `both` is more than one step from the darker of the two.
    u32 mismatched = 0;
    i32 worst = 0;
    /// Pixels the map darkens well past the mask, and the mask well past the map.
    u32 map_darker = 0;
    u32 mask_darker = 0;
};

/// Shading is monotonic in visibility, and the three frames differ in nothing else, so a frame
/// that takes the darker of the two visibilities is — channel for channel — the darker of the two
/// frames. A frame that multiplied them would be darker still wherever both are partial.
[[nodiscard]] DarkerOf darker_of(const std::vector<u32>& both, const std::vector<u32>& mask_only,
                                 const std::vector<u32>& map_only) noexcept {
    DarkerOf out;
    for (usize index = 0; index < both.size(); ++index) {
        i32 worst = 0;
        for (u32 which = 0; which < 3U; ++which) {
            const i32 expected =
                std::min(channel(mask_only[index], which), channel(map_only[index], which));
            worst = std::max(worst, std::abs(channel(both[index], which) - expected));
        }
        out.mismatched += worst > 1 ? 1U : 0U;
        out.worst = std::max(out.worst, worst);
        const i32 by_mask = brightness(mask_only[index]);
        const i32 by_map = brightness(map_only[index]);
        out.map_darker += by_map + 20 < by_mask ? 1U : 0U;
        out.mask_darker += by_mask + 20 < by_map ? 1U : 0U;
    }
    return out;
}

}  // namespace

CY_TEST_CASE("(a) a baked red wall bleeds onto the white surfaces near it, not those far away") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    RunOptions lit;
    lit.lightmap = Lightmap::Enabled;
    CornerRun off(fixture, RunOptions{});
    CornerRun on(fixture, lit);
    CY_REQUIRE(off.render());
    CY_REQUIRE(on.render());
    save("lightmaps-off.png", off.pixels());
    save("lightmaps-on.png", on.pixels());
    save_atlas("lightmaps-atlas.png", on.lightmap());

    const Picture picture = classify_all(on.scene());
    const Bleed flat = measure_bleed(picture, off.pixels());
    const Bleed baked = measure_bleed(picture, on.pixels());
    std::fprintf(stderr,
                 "(a) redness (red/green): off, wall near %.3f far %.3f, cube near %.3f far %.3f; "
                 "lightmapped, wall near %.3f far %.3f, cube near %.3f far %.3f\n",
                 flat.wall_near.redness(), flat.wall_far.redness(), flat.cube_near.redness(),
                 flat.cube_far.redness(), baked.wall_near.redness(), baked.wall_far.redness(),
                 baked.cube_near.redness(), baked.cube_far.redness());
    CY_REQUIRE(baked.wall_near.count > 200U);
    CY_REQUIRE(baked.wall_far.count > 200U);
    CY_REQUIRE(baked.cube_near.count > 100U);
    CY_REQUIRE(baked.cube_far.count > 100U);
    // OFF: the flat ambient on white, one colour near and far.
    CY_CHECK_LT(std::fabs(flat.wall_near.redness() - flat.wall_far.redness()), 0.01);
    // ON: the red wall tints what is near it and not what is across the room.
    CY_CHECK_GT(baked.wall_near.redness() - baked.wall_far.redness(), 0.1);
    CY_CHECK_GT(baked.cube_near.redness() - baked.cube_far.redness(), 0.1);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) the frame's lightmap term follows the host's sample_lightmap, pixel for pixel") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // AMBIENT ONLY: the frame drawn with no light, so every pixel is albedo times the lightmap.
    RunOptions ambient;
    ambient.lightmap = Lightmap::Enabled;
    ambient.frame_sun = false;
    CornerRun on(fixture, ambient);
    CY_REQUIRE(on.render());
    const Picture picture = classify_all(on.scene());
    const Span<const Aabb> boxes = on.scene().boxes();
    std::vector<f64> host_brightness;
    std::vector<f64> device_brightness;
    std::vector<f64> host_redness;
    std::vector<f64> device_redness;
    for (usize index = 0; index < picture.pixels.size(); index += 2) {
        const Pixel& pixel = picture.pixels[index];
        if (!picture.interior[index] || pixel.surface == Surface::None || pixel.box >= kUsedBoxes) {
            continue;
        }
        const Aabb& box = boxes[pixel.box];
        const u32 face = face_of(box, pixel.point);
        const Vec2 uv2 = uv2_of(box, pixel.point, face);
        const Vec3 ambient_radiance = bake::sample_lightmap(
            on.lightmap(), on.lightmap().addresses[pixel.box], uv2, kFaceNormals[face]);
        const Vec3 host = cwise_mul(albedo_of(pixel.box), ambient_radiance);
        const u32 texel = on.pixels()[index];
        host_brightness.push_back(static_cast<f64>(host.x + host.y + host.z));
        device_brightness.push_back(channel(texel, 0) + channel(texel, 1) + channel(texel, 2));
        host_redness.push_back(static_cast<f64>(host.x / std::max(host.y, 1.0e-6F)));
        device_redness.push_back(static_cast<f64>(channel(texel, 0)) /
                                 std::max(1.0, static_cast<f64>(channel(texel, 1))));
    }
    const f64 brightness = correlation(host_brightness, device_brightness);
    const f64 redness = correlation(host_redness, device_redness);
    std::fprintf(stderr,
                 "(b) over %zu pixels: host/device correlation %.4f in brightness, %.4f in "
                 "redness\n",
                 host_brightness.size(), brightness, redness);
    CY_REQUIRE(host_brightness.size() > 5000U);
    // Through the tonemap and 8 bits on one side and not the other, so a correlation.
    CY_CHECK_GT(brightness, 0.97);
    CY_CHECK_GT(redness, 0.97);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) no lightmap is the frame before lightmaps existed, byte for byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    RunOptions unaddressed;
    unaddressed.lightmap = Lightmap::Unaddressed;
    RunOptions probe_mode;
    probe_mode.lightmap = Lightmap::ProbeMode;
    RunOptions enabled;
    enabled.lightmap = Lightmap::Enabled;
    CornerRun absent(fixture, RunOptions{});
    CornerRun bound(fixture, unaddressed);
    CornerRun excluded(fixture, probe_mode);
    CornerRun on(fixture, enabled);
    CY_REQUIRE(absent.render());
    CY_REQUIRE(bound.render());
    CY_REQUIRE(excluded.render());
    CY_REQUIRE(on.render());
    CY_CHECK_EQ(bound.report().passes_declared, absent.report().passes_declared);
    std::fprintf(stderr,
                 "(c) %zu pixel(s) differ between no lightmap and one bound with no draw "
                 "addressed; %zu with every draw addressed under Probe; %zu when it is on\n",
                 differing(absent.pixels(), bound.pixels()),
                 differing(absent.pixels(), excluded.pixels()),
                 differing(absent.pixels(), on.pixels()));
    CY_CHECK_EQ(differing(absent.pixels(), bound.pixels()), usize{0});
    CY_CHECK_EQ(differing(absent.pixels(), excluded.pixels()), usize{0});
    // The control: on, the picture changes, or the two above would be equal for want of a lightmap.
    CY_CHECK_GT(differing(absent.pixels(), on.pixels()), usize{1000});
    save("lightmaps-absent.png", absent.pixels());
    check_against_before(absent.pixels());
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) with a volume bound too, a lightmapped surface takes the lightmap alone") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // The far cube carries no lightmap address: it is the dynamic object of the scene.
    RunOptions lightmap_only;
    lightmap_only.lightmap = Lightmap::Enabled;
    lightmap_only.unaddressed_box = kCubeFar;
    RunOptions both = lightmap_only;
    both.volume = true;
    CornerRun alone(fixture, lightmap_only);
    CornerRun combined(fixture, both);
    CY_REQUIRE(alone.render());
    CY_REQUIRE(combined.render());
    const Picture picture = classify_all(combined.scene());
    std::vector<bool> lightmapped(picture.pixels.size(), false);
    std::vector<bool> dynamic(picture.pixels.size(), false);
    u32 lightmapped_count = 0;
    u32 dynamic_count = 0;
    for (usize index = 0; index < picture.pixels.size(); ++index) {
        const Pixel& pixel = picture.pixels[index];
        if (!picture.interior[index] || pixel.surface == Surface::None || pixel.box >= kUsedBoxes) {
            continue;
        }
        if (pixel.box == kCubeFar) {
            dynamic[index] = true;
            ++dynamic_count;
        } else {
            lightmapped[index] = true;
            ++lightmapped_count;
        }
    }
    const i32 lightmapped_delta =
        worst_channel_delta(alone.pixels(), combined.pixels(), &lightmapped);
    const i32 dynamic_delta = worst_channel_delta(alone.pixels(), combined.pixels(), &dynamic);
    std::fprintf(stderr,
                 "(d) adding the volume: lightmapped pixels (%u) move by %d at most, the far "
                 "cube's (%u) by %d\n",
                 lightmapped_count, lightmapped_delta, dynamic_count, dynamic_delta);
    CY_REQUIRE(lightmapped_count > 5000U);
    CY_REQUIRE(dynamic_count > 100U);
    // The lightmapped surfaces do not see the volume at all...
    CY_CHECK_EQ(lightmapped_delta, 0);
    // ...and the one without a lightmap takes it in place of the flat ambient.
    CY_CHECK_GT(dynamic_delta, 2);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(e) the directional and SH L1 encodings draw irradiance at the geometric normal") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    RunOptions irradiance;
    irradiance.lightmap = Lightmap::Enabled;
    irradiance.directional_as_irradiance = true;
    RunOptions directional;
    directional.lightmap = Lightmap::Enabled;
    directional.mode = bake::LightmapMode::Directional;
    RunOptions sh;
    sh.lightmap = Lightmap::Enabled;
    sh.mode = bake::LightmapMode::ShL1;
    CornerRun first_plane(fixture, irradiance);
    CornerRun with_direction(fixture, directional);
    CornerRun with_sh(fixture, sh);
    CornerRun absent(fixture, RunOptions{});
    CY_REQUIRE(first_plane.render());
    CY_REQUIRE(with_direction.render());
    CY_REQUIRE(with_sh.render());
    CY_REQUIRE(absent.render());
    const Picture picture = classify_all(first_plane.scene());
    const i32 directional_delta =
        worst_channel_delta(first_plane.pixels(), with_direction.pixels(), &picture.interior);
    // The SH bake is its own run, with its own noise, so it is held to the frame's own term being
    // the host's rather than to the other bake: a mean over the picture against the absent frame.
    const usize sh_changed = differing(absent.pixels(), with_sh.pixels());
    std::fprintf(stderr,
                 "(e) directional against its own first plane: worst channel %d; SH L1 changes "
                 "%zu pixels\n",
                 directional_delta, sh_changed);
    // The factor is exactly one at the geometric normal, but half floats and a bilinear blend of
    // `w` sit between the two: one 8-bit step.
    CY_CHECK_LE(directional_delta, 1);
    CY_CHECK_GT(sh_changed, usize{1000});
    const i32 sh_delta =
        worst_channel_delta(with_direction.pixels(), with_sh.pixels(), &picture.interior);
    std::fprintf(stderr, "(e) SH L1 against directional, interior pixels: worst channel %d\n",
                 sh_delta);
    // Two bakes of the same light, each denoised: a few steps of noise, not a different picture.
    CY_CHECK_LE(sh_delta, 12);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(f) an unchanged lightmap is not uploaded again") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    bake::BakedLightmap lightmap;
    lightmap.mode = bake::LightmapMode::Irradiance;
    lightmap.page_size = 128;
    lightmap.pages = 1;
    lightmap.texels.width = 128;
    lightmap.texels.height = 128;
    lightmap.texels.planes = 1;
    CY_REQUIRE(lightmap.texels.texels.resize(usize{128} * 128).has_value());
    for (Vec4& texel : lightmap.texels.texels) {
        texel = Vec4{0.5F, 0.25F, 0.125F, 1.0F};
    }
    lightmaps::LightmapTextures textures;
    textures.initialize(fixture.device(), allocator());
    CY_REQUIRE(textures.upload(lightmap).has_value());
    CY_REQUIRE(textures.upload(lightmap).has_value());
    CY_CHECK_EQ(textures.uploads(), 1U);
    CY_CHECK_EQ(textures.device_bytes(), u64{128} * 128 * 8);
    lightmap.texels.texels[77].x = 2.0F;
    CY_REQUIRE(textures.upload(lightmap).has_value());
    CY_CHECK_EQ(textures.uploads(), 2U);
    textures.shutdown();
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(g) a stationary sun's direct term takes the baked shadow, at any intensity") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // THE SCENARIO "Stationary light with shadow mask": the sun is stationary, so the bake holds
    // its bounce and a mask channel, and the frame — with no shadow map bound — shades its direct
    // term itself. The two cubes cast the only shadows on the visible floor.
    RunOptions masked;
    masked.lightmap = Lightmap::Enabled;
    RunOptions unmasked = masked;  // the frame's sun matches no baked light: no baked shadow
    unmasked.match_lights = false;
    RunOptions dimmed = masked;  // moved in intensity at run time, nothing re-baked
    dimmed.sun_scale = 0.35F;
    RunOptions dimmed_unmasked = unmasked;
    dimmed_unmasked.sun_scale = 0.35F;
    CornerRun with_mask(fixture, masked);
    CornerRun without_mask(fixture, unmasked);
    CornerRun with_mask_dimmed(fixture, dimmed);
    CornerRun without_mask_dimmed(fixture, dimmed_unmasked);
    CY_REQUIRE(with_mask.render());
    CY_REQUIRE(without_mask.render());
    CY_REQUIRE(with_mask_dimmed.render());
    CY_REQUIRE(without_mask_dimmed.render());
    save("lightmaps-stationary-mask.png", with_mask.pixels());
    save("lightmaps-stationary-no-mask.png", without_mask.pixels());
    save("lightmaps-stationary-dimmed.png", with_mask_dimmed.pixels());

    const Picture picture = classify_all(with_mask.scene());
    const FloorShadow floor =
        floor_shadow(picture, with_mask.lightmap(), with_mask.scene().boxes());
    const Change masking_lit = change_over(without_mask.pixels(), with_mask.pixels(), floor.lit);
    const Change masking_shadowed =
        change_over(without_mask.pixels(), with_mask.pixels(), floor.shadowed);
    const Change dimming_shadowed =
        change_over(with_mask.pixels(), with_mask_dimmed.pixels(), floor.shadowed);
    const Change dimming_lit =
        change_over(with_mask.pixels(), with_mask_dimmed.pixels(), floor.lit);
    const Change dimming_unmasked =
        change_over(without_mask.pixels(), without_mask_dimmed.pixels(), floor.shadowed);
    std::fprintf(stderr,
                 "(g) floor: %zu pixels in the baked shadow, %zu fully lit. The mask moves the "
                 "shadowed ones by %.1f (worst %d) and the lit ones by worst %d. Dimming the sun "
                 "to 35%%: shadowed %.1f (worst %d), lit %.1f; with no mask the shadowed ones move "
                 "%.1f\n",
                 floor.shadowed.size(), floor.lit.size(), masking_shadowed.mean,
                 masking_shadowed.worst, masking_lit.worst, dimming_shadowed.mean,
                 dimming_shadowed.worst, dimming_lit.mean, dimming_unmasked.mean);
    CY_REQUIRE(floor.shadowed.size() > 50U);
    CY_REQUIRE(floor.lit.size() > 2000U);
    // The mask is what shadows the floor: it darkens the shadowed pixels and leaves the lit ones.
    CY_CHECK_LT(masking_shadowed.mean, -40.0);
    CY_CHECK_EQ(masking_lit.worst, 0);
    // Dimmed at run time, the lit floor dims and the shadowed floor — whose direct term the mask
    // zeroed — does not move at all: the baked shadow held at the new intensity.
    CY_CHECK_LT(dimming_lit.mean, -20.0);
    CY_CHECK_EQ(dimming_shadowed.worst, 0);
    // The control: without the mask the same pixels are lit, and dimming moves them.
    CY_CHECK_LT(dimming_unmasked.mean, -20.0);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "(h) a static sun's direct term is in the texels, and the frame does not add it again") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // The far cube has no lightmap: it is the object the frame still lights.
    RunOptions baked_sun;
    baked_sun.lightmap = Lightmap::Enabled;
    baked_sun.sun_mobility = gi::LightMobility::Static;
    baked_sun.unaddressed_box = kCubeFar;
    RunOptions no_frame_sun = baked_sun;
    no_frame_sun.frame_sun = false;
    RunOptions stationary_no_sun = no_frame_sun;
    stationary_no_sun.sun_mobility = gi::LightMobility::Stationary;
    CornerRun with_sun(fixture, baked_sun);
    CornerRun without_sun(fixture, no_frame_sun);
    CornerRun indirect_only(fixture, stationary_no_sun);
    CY_REQUIRE(with_sun.render());
    CY_REQUIRE(without_sun.render());
    CY_REQUIRE(indirect_only.render());
    save("lightmaps-static-sun.png", with_sun.pixels());

    const Picture picture = classify_all(with_sun.scene());
    std::vector<usize> lightmapped;
    std::vector<usize> dynamic;
    std::vector<usize> floor;
    for (usize index = 0; index < picture.pixels.size(); ++index) {
        const Pixel& pixel = picture.pixels[index];
        if (!picture.interior[index] || pixel.surface == Surface::None || pixel.box >= kUsedBoxes) {
            continue;
        }
        (pixel.box == kCubeFar ? dynamic : lightmapped).push_back(index);
        if (pixel.surface == Surface::Floor) {
            floor.push_back(index);
        }
    }
    const Change frame_sun = change_over(without_sun.pixels(), with_sun.pixels(), lightmapped);
    const Change dynamic_sun = change_over(without_sun.pixels(), with_sun.pixels(), dynamic);
    const Change baked_direct = change_over(indirect_only.pixels(), without_sun.pixels(), floor);
    std::fprintf(stderr,
                 "(h) the frame's static sun moves %zu lightmapped pixels by worst %d and the far "
                 "cube's %zu by %.1f (worst %d); the texels alone light the floor %.1f brighter "
                 "than an indirect-only bake\n",
                 lightmapped.size(), frame_sun.worst, dynamic.size(), dynamic_sun.mean,
                 dynamic_sun.worst, baked_direct.mean);
    CY_REQUIRE(lightmapped.size() > 5000U);
    CY_REQUIRE(dynamic.size() > 100U);
    // Not counted twice: the frame's sun adds nothing where its direct term is baked...
    CY_CHECK_EQ(frame_sun.worst, 0);
    // ...and still lights the surface that has no lightmap: its sunlit faces, which are a share
    // of the far cube's pixels (its front faces the camera and not the sun).
    CY_CHECK_GT(dynamic_sun.mean, 5.0);
    CY_CHECK_GT(dynamic_sun.worst, 60);
    // The texels do hold it.
    CY_CHECK_GT(baked_direct.mean, 20.0);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "(i) every level of the chain and of the mask reaches the device as the bake made it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    RunOptions lit;
    lit.lightmap = Lightmap::Enabled;
    lit.mode = bake::LightmapMode::Directional;
    CornerRun on(fixture, lit);
    CY_REQUIRE(on.render());
    const bake::BakedLightmap& lightmap = on.lightmap();
    lightmaps::LightmapTextures& textures = on.textures();
    CY_REQUIRE(lightmap.mip_levels >= 1U);
    CY_REQUIRE(textures.has_shadow_mask());
    CY_CHECK_EQ(textures.mip_levels(), lightmap.mip_levels + 1U);
    CY_CHECK_EQ(textures.device_bytes(), lightmap.device_bytes() + lightmap.shadow_mask_bytes());
    const u32 width = lightmap.texels.width;
    const u32 height = lightmap.texels.height;
    u32 compared = 0;
    for (u32 level = 0; level <= lightmap.mip_levels; ++level) {
        for (u32 plane = 0; plane <= lightmaps::kMaxPlanes; ++plane) {
            const bool mask = plane == lightmaps::kMaxPlanes;
            if (!mask && plane >= lightmap.texels.planes) {
                continue;
            }
            const bake::LightmapTexels& texels = mask ? bake::shadow_mask_level(lightmap, level)
                                                      : bake::lightmap_level(lightmap, level);
            const usize count = usize{texels.width} * texels.height;
            const std::vector<u16> expected = host_halves(
                Span<const Vec4>(texels.texels.data() + (mask ? 0U : count * plane), count));
            std::vector<u16> device;
            CY_REQUIRE(read_level(fixture.device(), textures, plane, width, height, level, device));
            CY_CHECK(device == expected);
            compared += 1U;
        }
    }
    std::fprintf(stderr,
                 "(i) %u levels of %u planes and the mask read back from the device, each the "
                 "host's halves byte for byte; %.1f KiB on the device\n",
                 lightmap.mip_levels + 1U, lightmap.texels.planes,
                 static_cast<double>(textures.device_bytes()) / 1024.0);
    CY_CHECK_EQ(compared, (lightmap.mip_levels + 1U) * (lightmap.texels.planes + 1U));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

/// What the density view draws on the back wall (resolution scale 1), the near cube (scale 4) and
/// the far cube, which has no lightmap.
struct DensityPicture {
    Tally wall;
    Tally cube;
    Tally unlit;
    /// The spread of the wall's green between its 10th and 90th percentiles: the checker.
    i32 checker_spread = 0;
};

[[nodiscard]] DensityPicture density_picture(CornerRun& run) {
    const Picture picture = classify_all(run.scene());
    DensityPicture out;
    std::vector<i32> wall_levels;
    for (usize index = 0; index < picture.pixels.size(); ++index) {
        const Pixel& pixel = picture.pixels[index];
        if (!picture.interior[index]) {
            continue;
        }
        const u32 texel = run.pixels()[index];
        if (pixel.surface == Surface::BackWall) {
            out.wall.add(texel);
            wall_levels.push_back(channel(texel, 1));
        } else if (pixel.surface == Surface::CubeFront && pixel.box == kCubeNear) {
            out.cube.add(texel);
        } else if (pixel.box == kCubeFar && pixel.surface != Surface::None) {
            out.unlit.add(texel);
        }
    }
    std::ranges::sort(wall_levels);
    out.checker_spread = wall_levels.empty() ? 0
                                             : wall_levels[(wall_levels.size() * 9U) / 10U] -
                                                   wall_levels[wall_levels.size() / 10U];
    return out;
}

[[nodiscard]] Vec3 mean_of(const Tally& tally) noexcept {
    const f64 count = static_cast<f64>(std::max(tally.count, 1U));
    return Vec3{static_cast<f32>(tally.red / count), static_cast<f32>(tally.green / count),
                static_cast<f32>(tally.blue / count)};
}

CY_TEST_CASE(
    "(j) the texel-density view colours each surface by the density its lightmap gives it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    RunOptions density;
    density.lightmap = Lightmap::Enabled;
    density.density_view = true;
    density.unaddressed_box = kCubeFar;
    // The same bake against four times the target: the wall is two octaves under it.
    RunOptions demanding = density;
    demanding.density_target = kTexelDensity * 4.0F;
    // The same view two stops brighter: the view divides the exposure back out.
    RunOptions brighter = density;
    brighter.exposure_shift = 2.0F;
    CornerRun view(fixture, density);
    CornerRun under(fixture, demanding);
    CornerRun exposed(fixture, brighter);
    CY_REQUIRE(view.render());
    CY_REQUIRE(under.render());
    CY_REQUIRE(exposed.render());
    save("lightmaps-density.png", view.pixels());
    const DensityPicture at_target = density_picture(view);
    const DensityPicture below = density_picture(under);
    const Vec3 wall = mean_of(at_target.wall);
    const Vec3 cube = mean_of(at_target.cube);
    const Vec3 unlit = mean_of(at_target.unlit);
    const Vec3 wall_under = mean_of(below.wall);
    std::vector<usize> every_pixel(view.pixels().size());
    std::iota(every_pixel.begin(), every_pixel.end(), usize{0});
    const Change exposure = change_over(view.pixels(), exposed.pixels(), every_pixel);
    std::fprintf(stderr,
                 "(j) back wall (resolution scale 1) rgb %.0f %.0f %.0f, checker spread %d; near "
                 "cube (scale 4) rgb %.0f %.0f %.0f; unlightmapped far cube rgb %.0f %.0f %.0f; "
                 "the wall at four times the target rgb %.0f %.0f %.0f; two stops of exposure "
                 "move a pixel by worst %d\n",
                 static_cast<f64>(wall.x), static_cast<f64>(wall.y), static_cast<f64>(wall.z),
                 at_target.checker_spread, static_cast<f64>(cube.x), static_cast<f64>(cube.y),
                 static_cast<f64>(cube.z), static_cast<f64>(unlit.x), static_cast<f64>(unlit.y),
                 static_cast<f64>(unlit.z), static_cast<f64>(wall_under.x),
                 static_cast<f64>(wall_under.y), static_cast<f64>(wall_under.z), exposure.worst);
    CY_REQUIRE(at_target.wall.count > 200U);
    CY_REQUIRE(at_target.cube.count > 100U);
    CY_REQUIRE(at_target.unlit.count > 100U);
    CY_REQUIRE(below.wall.count > 200U);
    // At the level's density the wall is green; the cube, baked at four times it, is red.
    CY_CHECK_GT(wall.y, 2.0F * wall.x);
    CY_CHECK_GT(wall.y, 2.0F * wall.z);
    CY_CHECK_GT(cube.x, 2.0F * cube.y);
    // Against four times the target the same wall is under it: blue.
    CY_CHECK_GT(wall_under.z, 2.0F * wall_under.x);
    CY_CHECK_GT(wall_under.z, 2.0F * wall_under.y);
    // Each lightmap texel is a cell of the checker: the wall shows two levels, not one.
    CY_CHECK_GT(at_target.checker_spread, 20);
    // A surface with no lightmap is flat grey.
    CY_CHECK_LT(std::fabs(unlit.x - unlit.y), (0.02F * unlit.x) + 1.0F);
    CY_CHECK_LT(std::fabs(unlit.y - unlit.z), (0.02F * unlit.y) + 1.0F);
    // The view's colours are the same at any exposure.
    CY_CHECK_LE(exposure.worst, 1);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "(k) a stationary point light's direct term takes its own mask channel, through the "
    "cluster lists") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // The lamp is the mask's SECOND channel but, with the sun left out of the frame, the frame's
    // FIRST light, so the frame's words map one to the other; and it is a point light, so its
    // direct term goes through the cluster walk, not the directional loop (g) holds.
    RunOptions lit;
    lit.lightmap = Lightmap::Enabled;
    lit.lamp = true;
    lit.frame_sun = false;  // the lamp is the frame's only light, index 0 against channel 1
    RunOptions dimmed = lit;
    dimmed.lamp_scale = 0.3F;
    RunOptions unmatched = lit;
    unmatched.match_lights = false;
    RunOptions unmatched_dimmed = unmatched;
    unmatched_dimmed.lamp_scale = 0.3F;
    CornerRun full(fixture, lit);
    CornerRun low(fixture, dimmed);
    CornerRun plain(fixture, unmatched);
    CornerRun plain_low(fixture, unmatched_dimmed);
    CY_REQUIRE(full.render());
    CY_REQUIRE(low.render());
    CY_REQUIRE(plain.render());
    CY_REQUIRE(plain_low.render());
    save("lightmaps-stationary-lamp.png", full.pixels());
    CY_REQUIRE_EQ(full.lightmap().shadow_lights.size(), 2U);
    CY_CHECK_EQ(full.lightmap().shadow_lights[1], kLampId);

    const Picture picture = classify_all(full.scene());
    const FloorShadow lamp = floor_shadow(picture, full.lightmap(), full.scene().boxes(), 1U);
    const Change dimming_shadowed = change_over(full.pixels(), low.pixels(), lamp.shadowed);
    const Change dimming_lit = change_over(full.pixels(), low.pixels(), lamp.lit);
    const Change unmatched_shadowed =
        change_over(plain.pixels(), plain_low.pixels(), lamp.shadowed);
    std::fprintf(stderr,
                 "(k) floor: %zu pixels in the lamp's baked shadow, %zu in its full light. "
                 "Dimming the lamp to 30%%: shadowed %.1f (worst %d), lit %.1f; unmatched to the "
                 "bake, the shadowed ones move %.1f\n",
                 lamp.shadowed.size(), lamp.lit.size(), dimming_shadowed.mean,
                 dimming_shadowed.worst, dimming_lit.mean, unmatched_shadowed.mean);
    CY_REQUIRE(lamp.shadowed.size() > 50U);
    CY_REQUIRE(lamp.lit.size() > 1000U);
    CY_CHECK_EQ(dimming_shadowed.worst, 0);
    CY_CHECK_LT(dimming_lit.mean, -10.0);
    CY_CHECK_LT(unmatched_shadowed.mean, -10.0);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(l) a minified surface reads the coarser levels of the uploaded chain") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    RunOptions probe;
    probe.lightmap = Lightmap::Enabled;
    probe.mip_probe = true;
    probe.frame_sun = false;  // ambient only: the floor is albedo times the probe
    CornerRun run(fixture, probe);
    CY_REQUIRE(run.render());
    save("lightmaps-mip-probe.png", run.pixels());
    const Picture picture = classify_all(run.scene());
    u32 near_floor = 0;
    u32 near_red = 0;
    u32 far_floor = 0;
    u32 far_red = 0;
    for (usize index = 0; index < picture.pixels.size(); ++index) {
        const Pixel& pixel = picture.pixels[index];
        if (!picture.interior[index] || pixel.surface != Surface::Floor) {
            continue;
        }
        const u32 texel = run.pixels()[index];
        const bool red = channel(texel, 0) > channel(texel, 1) + 20;
        // Near: the floor within 4.5 m of the eye, the nearest the camera sees, where a pixel
        // covers about a texel. Far: beyond 6.5 m, where the grazing floor covers several texels
        // a pixel. Between the two the trilinear blend turns the floor from grey to red.
        const f32 distance = length(pixel.point);
        if (distance < 4.5F) {
            ++near_floor;
            near_red += red ? 1U : 0U;
        } else if (distance > 6.5F) {
            ++far_floor;
            far_red += red ? 1U : 0U;
        }
    }
    std::fprintf(stderr,
                 "(l) the probe's coarser levels are red: %u of %u far floor pixels read them, %u "
                 "of %u near ones\n",
                 far_red, far_floor, near_red, near_floor);
    CY_REQUIRE(far_floor > 500U);
    CY_REQUIRE(near_floor > 100U);
    // The far floor minifies into the chain; the near floor is magnified and reads level 0 alone.
    CY_CHECK_GT(far_red, far_floor / 2U);
    CY_CHECK_EQ(near_red, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "(m) a stationary sun takes the darker of its mask channel and the real-time shadow map") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // THE SPEC'S "the darker of that channel and any real-time shadow the frame has for the light".
    // The sun's real-time map is rendered and bound, and a movable box hangs over the lit floor: it
    // is in no bake, so only the map sees its shadow, while the cubes' shadows are in both. Three
    // frames of one scene: the mask alone (no map), the map alone (the frame's sun matches no baked
    // light, so no mask), and both.
    RunOptions both;
    both.lightmap = Lightmap::Enabled;
    both.shadow_map = true;
    both.occluder = true;
    RunOptions mask_only = both;
    mask_only.shadow_map = false;
    RunOptions map_only = both;
    map_only.match_lights = false;
    CornerRun with_both(fixture, both);
    CornerRun with_mask(fixture, mask_only);
    CornerRun with_map(fixture, map_only);
    CY_REQUIRE(with_both.render());
    CY_REQUIRE(with_mask.render());
    CY_REQUIRE(with_map.render());
    save("lightmaps-stationary-realtime.png", with_both.pixels());
    save("lightmaps-stationary-realtime-map.png", with_map.pixels());
    save("lightmaps-stationary-realtime-mask.png", with_mask.pixels());

    const DarkerOf darker = darker_of(with_both.pixels(), with_mask.pixels(), with_map.pixels());
    std::fprintf(stderr,
                 "(m) the map darkens %u pixels past the mask and the mask %u past the map; the "
                 "frame through both is the darker of the two at all but %u pixels (worst %d)\n",
                 darker.map_darker, darker.mask_darker, darker.mismatched, darker.worst);
    // The movable box's shadow is the map's alone: a frame that ignored the map on a lightmapped
    // surface would leave it lit.
    CY_REQUIRE(darker.map_darker > 500U);
    // And the frame through both is, pixel for pixel, the darker of the two.
    CY_CHECK_EQ(darker.mismatched, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(n) the frame's time without the lightmap, with it, and with its mask") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // A MEASUREMENT, NOT A BUDGET, and not a GPU time: each frame from the start of its recording
    // to the device going idle, on the host's clock. The same corner three ways — no lightmap; the
    // lightmap with a static sun, so no mask; and the lightmap with a stationary sun, its mask
    // sampled and its channel matched — one frame of each in turn, so a drift in the machine's
    // clocks falls on all three alike. Every other pass is the same in the three, so the
    // differences in the medians are what the lightmap costs, the host's share included.
    constexpr u32 kFrames = 96;
    constexpr u32 kRuns = 3;
    RunOptions configurations[kRuns];
    configurations[1].lightmap = Lightmap::Enabled;
    configurations[1].sun_mobility = gi::LightMobility::Static;
    configurations[2].lightmap = Lightmap::Enabled;
    const char* names[kRuns] = {"no lightmap", "lightmap, static sun, no mask",
                                "lightmap, stationary sun through its mask"};
    std::vector<std::unique_ptr<CornerRun>> runs;
    for (const RunOptions& options : configurations) {
        runs.push_back(std::make_unique<CornerRun>(fixture, options));
        CY_REQUIRE(runs.back()->render());
    }
    std::vector<f64> frames[kRuns];
    for (u32 frame = 0; frame < kFrames; ++frame) {
        for (u32 which = 0; which < kRuns; ++which) {
            f64 nanoseconds = 0.0;
            CY_REQUIRE(runs[which]->time_frame(nanoseconds));
            frames[which].push_back(nanoseconds);
        }
    }
    f64 medians[kRuns] = {};
    for (u32 which = 0; which < kRuns; ++which) {
        std::vector<f64>& times = frames[which];
        std::ranges::sort(times);
        medians[which] = times[times.size() / 2U];
        std::fprintf(stderr,
                     "(n) %s: median %.1f us, quartiles %.1f to %.1f us, over %u frames at %ux%u\n",
                     names[which], medians[which] / 1000.0, times[times.size() / 4U] / 1000.0,
                     times[(times.size() * 3U) / 4U] / 1000.0, kFrames, kWidth, kHeight);
        CY_CHECK_GT(times.front(), 0.0);
    }
    std::fprintf(stderr,
                 "(n) the lightmap moves the median by %+.1f us, and its mask by %+.1f us\n",
                 (medians[1] - medians[0]) / 1000.0, (medians[2] - medians[1]) / 1000.0);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
