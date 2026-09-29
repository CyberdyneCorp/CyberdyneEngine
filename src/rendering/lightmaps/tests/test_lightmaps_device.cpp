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
//   (f) an unchanged lightmap is not uploaded again.
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
#include <cy/rendering/light_probes/probe_volume_texture.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
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
        return device_.has_value() &&
               device_.value()->capabilities().backend() == kBackend;
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

void place_box(u32 which, Vec3& centre, f32& half, void*) noexcept {
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

// --- The cube's lightmap coordinates ------------------------------------------------------------

/// `pipeline_test::CubeMesh`'s faces, in its order, and the tangent it builds each from.
constexpr Vec3 kFaceNormals[6] = {Vec3{1, 0, 0},  Vec3{-1, 0, 0}, Vec3{0, 1, 0},
                                  Vec3{0, -1, 0}, Vec3{0, 0, 1},  Vec3{0, 0, -1}};
/// Each face's cell leaves this share of the cell on every side as chart padding. With the
/// resolution scales `bake_corner` gives, two cells are at least `required_chart_gap` — four texels,
/// two of the one mip level a 256 page protects — apart on every box, which is what keeps a
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

/// The corner baked once per encoding and kept for the whole run: a bake is the expensive thing
/// here, and every case bakes the same boxes under the same sky.
struct CornerBake {
    bake::BakedLightmap lightmap;
    bake::LightmapBakeReport report;
    f64 seconds = 0.0;
};

[[nodiscard]] const CornerBake* bake_corner(Span<const Aabb> boxes, Vec3 flat,
                                            bake::LightmapMode mode,
                                            gi::LightMobility mobility) {
    static std::unique_ptr<CornerBake> baked[6];
    const bool baked_direct = mobility == gi::LightMobility::Static;
    std::unique_ptr<CornerBake>& slot =
        baked[(static_cast<u32>(mode) * 2U) + (baked_direct ? 1U : 0U)];
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
    const gi::GiLight sun = gi_sun(mobility);
    bake::LightmapScene scene;
    scene.meshes = {&mesh, 1};
    scene.materials = {materials.data(), materials.size()};
    scene.instances = {instances.data(), instances.size()};
    scene.lights = {&sun, 1};
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
};

struct Corner {
    FrameScene* scene = nullptr;
    RunOptions options;
    render::LightDescription sun;
    std::vector<pipeline::InstanceTransform> instances;
    std::vector<u8> materials;
    std::vector<rendering::GpuDrawInstance> draws;
    const bake::BakedLightmap* lightmap = nullptr;
    lightmaps::LightmapTextures* textures = nullptr;
    light_probes::ProbeVolumeTexture* volume_texture = nullptr;
    const gi::IrradianceVolume* volume = nullptr;
    Vec3 flat{0.0F, 0.0F, 0.0F};
    bool attach = false;
};

void configure_corner(rendering::assembly::AssemblyDescription& description, void*) noexcept {
    description.pin_jitter = true;
    description.post.ambient_occlusion = false;
}

Status before_assemble(rendering::RenderGraph&, rendering::assembly::AssemblyView& view,
                       rendering::assembly::FrameSinks&, void* user) noexcept {
    auto* corner = static_cast<Corner*>(user);
    view.lights = corner->options.frame_sun ? Span<const render::LightDescription>(&corner->sun, 1)
                                            : Span<const render::LightDescription>();
    view.sun_direction = normalize(-kSunTravel);
    view.cut = true;
    return ok();
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

Status before_upload(pipeline::FrameUpload& upload, void* user) noexcept {
    auto* corner = static_cast<Corner*>(user);
    whiten(*corner, upload);
    upload.globals.exposure_stops += kExposureShift;
    corner->flat = Vec3{upload.view.ambient_and_occlusion[0], upload.view.ambient_and_occlusion[1],
                        upload.view.ambient_and_occlusion[2]};
    if (!corner->attach) {
        return ok();
    }
    pipeline::MaterialTextureSlot slots[lightmaps::kMaxPlanes + 2];
    u32 count = 0;
    lightmaps::LightmapSlots lightmap_slots;
    for (u32 plane = 0; plane < corner->textures->planes(); ++plane) {
        lightmap_slots.planes[plane] = kLightmapSlot + plane;
        slots[count++] = corner->textures->slot(plane, kLightmapSlot + plane);
    }
    if (corner->textures->has_shadow_mask()) {
        lightmap_slots.shadow_mask = kShadowMaskSlot;
        slots[count++] = corner->textures->shadow_mask_slot(kShadowMaskSlot);
    }
    if (corner->options.volume) {
        slots[count++] = corner->volume_texture->slot(kVolumeSlot);
    }
    if (Status bound = corner->scene->set_frame_textures(
            Span<const pipeline::MaterialTextureSlot>(slots, count));
        !bound) {
        return bound;
    }
    if (corner->options.volume) {
        light_probes::write_probe_volume(kVolumeSlot, *corner->volume,
                                         corner->volume_texture->layout(), Vec3{}, upload.view);
    }
    const gi::GiMode mode =
        corner->options.lightmap == Lightmap::ProbeMode ? gi::GiMode::Probe : gi::GiMode::Baked;
    const u64 frame_lights[1] = {corner->options.match_lights ? corner->sun.stable_id : 999U};
    if (Status written = lightmaps::write_lightmaps(
            lightmap_slots, *corner->lightmap, mode,
            Span<const u64>(frame_lights, corner->options.frame_sun ? 1U : 0U), upload.view);
        !written) {
        return written;
    }
    if (corner->options.density_view) {
        lightmaps::write_lightmap_density_view(kTexelDensity, upload.view);
    }
    if (corner->options.lightmap != Lightmap::Unaddressed) {
        address_draws(*corner, upload);
    }
    return ok();
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
        corner_.sun.kind = render::LightKind::Directional;
        corner_.sun.intensity = kSunLux * options.sun_scale;
        corner_.sun.transform.rotation =
            Quat::look_rotation(normalize(kSunTravel), Vec3{0.0F, 1.0F, 0.0F});
        corner_.sun.stable_id = 1;
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
        ready_ = built.has_value();
        scene_.set_read_back(true);
        textures_.initialize(fixture.device(), allocator());
        volume_texture_.initialize(fixture.device(), allocator());
    }

    ~CornerRun() {
        if (!uv_buffer_.is_null()) {
            (void)device_->wait_idle();
            device_->destroy_buffer(uv_buffer_);
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

    /// The bake, the planes, the fourth stream and — if asked for — the volume.
    [[nodiscard]] bool attach() {
        const bake::LightmapMode mode = corner_.options.directional_as_irradiance
                                            ? bake::LightmapMode::Directional
                                            : corner_.options.mode;
        const CornerBake* baked =
            bake_corner(scene_.boxes(), corner_.flat, mode, corner_.options.sun_mobility);
        if (baked == nullptr) {
            return false;
        }
        corner_.lightmap = &baked->lightmap;
        if (corner_.options.directional_as_irradiance) {
            first_plane_ = std::make_unique<bake::BakedLightmap>();
            first_plane_->mode = bake::LightmapMode::Irradiance;
            first_plane_->page_size = baked->lightmap.page_size;
            first_plane_->pages = baked->lightmap.pages;
            first_plane_->gutter_texels = baked->lightmap.gutter_texels;
            first_plane_->texels.width = baked->lightmap.texels.width;
            first_plane_->texels.height = baked->lightmap.texels.height;
            first_plane_->texels.planes = 1;
            const usize count = usize{first_plane_->texels.width} * first_plane_->texels.height;
            if (!first_plane_->texels.texels
                     .append(Span<const Vec4>(baked->lightmap.texels.texels.data(), count))
                     .has_value() ||
                !first_plane_->addresses.append(baked->lightmap.addresses.span()).has_value()) {
                return false;
            }
            // The same shadow mask, every level, and the same lights: the case compares the
            // encodings, so everything else is the directional bake's own.
            if (!copy_texels(baked->lightmap.shadow_mask, first_plane_->shadow_mask) ||
                !first_plane_->shadow_lights.append(baked->lightmap.shadow_lights.span())
                     .has_value() ||
                !first_plane_->direct_lights.append(baked->lightmap.direct_lights.span())
                     .has_value()) {
                return false;
            }
            for (u32 level = 0; level < baked->lightmap.mip_levels; ++level) {
                if (!copy_texels(baked->lightmap.mip_shadow_mask[level],
                                 first_plane_->mip_shadow_mask[level])) {
                    return false;
                }
            }
            // And the first plane of every level of the chain, which the frame minifies into.
            first_plane_->mip_levels = baked->lightmap.mip_levels;
            for (u32 level = 0; level < baked->lightmap.mip_levels; ++level) {
                const bake::LightmapTexels& mip = baked->lightmap.mip_texels[level];
                bake::LightmapTexels& out = first_plane_->mip_texels[level];
                out.width = mip.width;
                out.height = mip.height;
                out.planes = 1;
                if (!out.texels
                         .append(Span<const Vec4>(mip.texels.data(),
                                                  usize{mip.width} * mip.height))
                         .has_value()) {
                    return false;
                }
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
/// reference the frame's mask read is held to. The sun is the mask's only light, channel 0.
[[nodiscard]] f32 host_mask(const bake::BakedLightmap& lightmap, Span<const Aabb> boxes,
                            const Pixel& pixel, u32 level) noexcept {
    const Aabb& box = boxes[pixel.box];
    const u32 face = face_of(box, pixel.point);
    const Vec2 coordinate =
        bake::atlas_coordinate(lightmap.addresses[pixel.box], uv2_of(box, pixel.point, face),
                               lightmap.page_size, lightmap.gutter_texels);
    const f32 scale = 1.0F / static_cast<f32>(1U << level);
    return bake::sample_plane(bake::shadow_mask_level(lightmap, level), 0, coordinate * scale).x;
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
    return footprint < 1.6F ? 0U : std::min(lightmap.mip_levels, 1U + static_cast<u32>(std::log2(footprint)));
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
                                   Span<const Aabb> boxes, usize index) noexcept {
    const Pixel& pixel = picture.pixels[index];
    if (!picture.interior[index] || pixel.surface != Surface::Floor) {
        return MaskState::Neither;
    }
    bool shadowed = true;
    bool lit = true;
    const u32 deepest = deepest_level(picture, lightmap, boxes, index);
    for (u32 level = 0; level <= deepest; ++level) {
        const f32 mask = host_mask(lightmap, boxes, pixel, level);
        shadowed = shadowed && mask == 0.0F;
        lit = lit && mask == 1.0F;
    }
    return shadowed ? MaskState::Shadowed : (lit ? MaskState::Lit : MaskState::Neither);
}

/// The floor pixels deep in the baked shadow and deep in the light: every pixel of their 3 x 3
/// neighbourhood reads an exact mask of the same value at its centre. The neighbourhood is the
/// margin for where the frame's pinned jitter actually samples the pixel, half a pixel away.
[[nodiscard]] FloorShadow floor_shadow(const Picture& picture, const bake::BakedLightmap& lightmap,
                                       Span<const Aabb> boxes) {
    std::vector<MaskState> states(picture.pixels.size(), MaskState::Neither);
    for (usize index = 0; index < picture.pixels.size(); ++index) {
        states[index] = mask_state(picture, lightmap, boxes, index);
    }
    FloorShadow out;
    for (u32 y = 1; y + 1U < kHeight; ++y) {
        for (u32 x = 1; x + 1U < kWidth; ++x) {
            const usize index = (usize{y} * kWidth) + x;
            bool uniform = states[index] != MaskState::Neither;
            for (u32 dy = 0; dy < 3U && uniform; ++dy) {
                for (u32 dx = 0; dx < 3U && uniform; ++dx) {
                    uniform = states[(usize{y + dy - 1U} * kWidth) + (x + dx - 1U)] ==
                              states[index];
                }
            }
            if (uniform) {
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
                              u32 plane, u32 width, u32 height, u32 level,
                              std::vector<u16>& out) {
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
    const FloorShadow floor = floor_shadow(picture, with_mask.lightmap(), with_mask.scene().boxes());
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

CY_TEST_CASE("(h) a static sun's direct term is in the texels, and the frame does not add it again") {
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

CY_TEST_CASE("(i) every level of the chain and of the mask reaches the device as the bake made it") {
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
            const std::vector<u16> expected = host_halves(Span<const Vec4>(
                texels.texels.data() + (mask ? 0U : count * plane), count));
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

CY_TEST_CASE("(j) the texel-density view colours each surface by the density its lightmap gives it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    RunOptions density;
    density.lightmap = Lightmap::Enabled;
    density.density_view = true;
    density.unaddressed_box = kCubeFar;
    CornerRun view(fixture, density);
    CY_REQUIRE(view.render());
    save("lightmaps-density.png", view.pixels());
    const Picture picture = classify_all(view.scene());
    Tally wall;
    Tally cube;
    Tally unlit;
    std::vector<i32> wall_levels;
    for (usize index = 0; index < picture.pixels.size(); ++index) {
        const Pixel& pixel = picture.pixels[index];
        if (!picture.interior[index]) {
            continue;
        }
        const u32 texel = view.pixels()[index];
        if (pixel.surface == Surface::BackWall) {
            wall.add(texel);
            wall_levels.push_back(channel(texel, 1));
        } else if (pixel.surface == Surface::CubeFront && pixel.box == kCubeNear) {
            cube.add(texel);
        } else if (pixel.box == kCubeFar && pixel.surface != Surface::None) {
            unlit.add(texel);
        }
    }
    std::sort(wall_levels.begin(), wall_levels.end());
    const i32 checker_spread = wall_levels.empty()
                                   ? 0
                                   : wall_levels[(wall_levels.size() * 9U) / 10U] -
                                         wall_levels[wall_levels.size() / 10U];
    const auto mean = [](const Tally& tally, f64 total) {
        return total / static_cast<f64>(std::max(tally.count, 1U));
    };
    std::fprintf(stderr,
                 "(j) back wall (resolution scale 1) rgb %.0f %.0f %.0f, checker spread %d; near "
                 "cube (scale 4) rgb %.0f %.0f %.0f; unlightmapped far cube rgb %.0f %.0f %.0f\n",
                 mean(wall, wall.red), mean(wall, wall.green), mean(wall, wall.blue),
                 checker_spread, mean(cube, cube.red), mean(cube, cube.green),
                 mean(cube, cube.blue), mean(unlit, unlit.red), mean(unlit, unlit.green),
                 mean(unlit, unlit.blue));
    CY_REQUIRE(wall.count > 200U);
    CY_REQUIRE(cube.count > 100U);
    CY_REQUIRE(unlit.count > 100U);
    // At the level's density the wall is green; the cube, baked at four times it, is red.
    CY_CHECK_GT(wall.green, 2.0 * wall.red);
    CY_CHECK_GT(wall.green, 2.0 * wall.blue);
    CY_CHECK_GT(cube.red, 2.0 * cube.green);
    // Each lightmap texel is a cell of the checker: the wall shows two levels, not one.
    CY_CHECK_GT(checker_spread, 20);
    // A surface with no lightmap is flat grey.
    CY_CHECK_LT(std::fabs(unlit.red - unlit.green), 0.02 * unlit.red + 1.0);
    CY_CHECK_LT(std::fabs(unlit.green - unlit.blue), 0.02 * unlit.green + 1.0);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
