// SPDX-License-Identifier: MIT
// An irradiance volume in the forward frame, on a Vulkan device with validation and synchronisation
// validation on. `render.light_probes`.
//
// ================================================================================================
// THE SCENE
// ================================================================================================
//
// `pipeline_test::FrameScene` — the one scene the pipeline suites render — rearranged through its
// hooks into a corner: the white floor slab, a red wall standing on it at x = -2.4, a white back
// wall at z = -9, and two white cubes on the floor, one beside the red wall and one far from it.
// One sun, travelling toward -x with no z component, so the back wall and the cubes' fronts (which
// face +z, the camera) receive NO direct light: what those pixels show is the ambient term alone.
// Every material is white and each instance's tint is its albedo.
//
// The volume is captured on the host from `gi::BoxProxyScene` built from the same boxes, the same
// sun, and a uniform sky at the frame's own flat ambient — so where a probe sees only sky, the
// volume reproduces the flat ambient exactly.
//
// ================================================================================================
// THE CASES
// ================================================================================================
//
//   (a) the back wall and the cube near the red wall are redder with the volume on, and the far
//       ones are not; with it off every one of them has the flat ambient's colour;
//   (b) the unlit back wall, uniform to within one 8-bit step under the flat ambient, varies under
//       the volume and is brightest at its foot, where the sunlit floor fills its view;
//   (c) the frame's term follows the host's `IrradianceVolume::ambient` pixel for pixel: the
//       correlation between the two over the back wall, in brightness and in redness;
//   (d) the volume attached but switched off is the frame with no volume, byte for byte, with the
//       same passes; and that frame is a committed reference drawn by the frame shader as it was
//       before this change (the SPIR-V at 0f1dfd1).
//
// Every frame is the SECOND frame of its scene with the temporal history cut, so the first frame —
// which the flat ambient the capture needs is read from — never blends into what is measured.

#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/irradiance_volume.h>
#include <cy/rendering/gi/proxy_scene.h>
#include <cy/rendering/light_probes/probe_volume_texture.h>
#include <cy/rendering/occlusion/gtao.h>
#include <cy/rendering/occlusion/occlusion_pass.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace cy;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
namespace gi = cy::rendering::gi;
namespace light_probes = cy::rendering::light_probes;

namespace {

/// The frame's slots of set 0's texture table the volume and the ambient occlusion term are bound
/// at. Any free slots.
constexpr u32 kVolumeSlot = 121;
constexpr u32 kOcclusionSlot = 120;
constexpr f32 kFloorTop = -1.9F + 0.125F;
constexpr f32 kBackWall = -9.0F;
constexpr f32 kRedWall = -2.4F;
constexpr Vec3 kSunTravel{-0.6F, -0.8F, 0.0F};
/// A midday sun, `render::LightDescription`'s own default. The pipeline scene's 22 000 lux under
/// the frame's flat ambient left the sky brighter than anything the sun could bounce, and a picture
/// in which bounce light cannot be seen is a poor test of it.
constexpr f32 kSunLux = 100000.0F;
/// The pipeline scene's exposure is set for its 22 000 lux sun; this is the same exposure moved by
/// the log2 of the ratio, so a sunlit white surface photographs as it does there.
constexpr f32 kExposureShift = -2.18F;
constexpr f32 kWhite = 0.8F;
constexpr Vec3 kRed{0.85F, 0.08F, 0.06F};

/// Which boxes the corner is made of. Box 0 is the floor slab and is where the scene put it.
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
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_light_probes";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        device_ = rhi::create_device(allocator_, "vulkan", description, selection_);
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
               device_.value()->capabilities().backend() == rhi::BackendKind::Vulkan;
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

/// `red_wall` false paints the red wall white: the control that separates what the red wall
/// bleeds from what any sunlit wall bounces.
Vec3 albedo_of(u32 box, bool red_wall) noexcept {
    return box == kRedWallBox && red_wall ? kRed : Vec3{kWhite, kWhite, kWhite};
}

// --- The corner, through the scene's hooks ------------------------------------------------------

enum class Volume : u8 {
    /// No volume anywhere: the frame every caller that predates it draws.
    Absent,
    /// Captured, uploaded and bound in the texture table, and the frame told there is none.
    Disabled,
    Enabled,
};

struct Corner {
    FrameScene* scene = nullptr;
    Volume volume = Volume::Absent;
    light_probes::ProbeVolumeTexture* texture = nullptr;
    const gi::IrradianceVolume* probes = nullptr;
    render::LightDescription sun;
    std::vector<rendering::pipeline::InstanceTransform> instances;
    std::vector<u8> materials;
    /// Ambient occlusion on as well, through `render.ambient_occlusion`'s own pass.
    rendering::occlusion::AmbientOcclusionPass* occlusion = nullptr;
    /// The frame's flat ambient, read off the first frame's upload.
    Vec3 flat{0.0F, 0.0F, 0.0F};
    bool attach = false;
    bool red_wall = true;
    /// False draws the frame with no light, for a picture of the ambient term alone. The volume is
    /// still captured under the sun.
    bool frame_sun = true;
};

void configure_corner(rendering::assembly::AssemblyDescription& description, void* user) noexcept {
    const auto* corner = static_cast<const Corner*>(user);
    description.pin_jitter = true;
    description.post.ambient_occlusion = corner->occlusion != nullptr;
}

Status set_occlusion_view(Corner& corner, Vec2 jitter) noexcept {
    rendering::occlusion::GtaoView view;
    view.projection = corner.scene->projection();
    view.relative_to_view = corner.scene->view();
    view.width = kWidth;
    view.height = kHeight;
    view.jitter = jitter;
    return corner.occlusion->set_view(view);
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
            // Behind the camera and below the floor: culled, and never in a ray's path.
            half = 0.3F;
            centre = Vec3{0.0F, -40.0F, 40.0F};
            return;
    }
}

Status before_assemble(rendering::RenderGraph& graph, rendering::assembly::AssemblyView& view,
                       rendering::assembly::FrameSinks& sinks, void* user) noexcept {
    auto* corner = static_cast<Corner*>(user);
    view.lights = corner->frame_sun ? Span<const render::LightDescription>(&corner->sun, 1)
                                    : Span<const render::LightDescription>();
    // The sky's own sun is this one, so its irradiance is the sky this sun would light.
    view.sun_direction = normalize(-kSunTravel);
    // Every frame a cut: what is measured is never blended with the frame before it.
    view.cut = true;
    if (corner->occlusion == nullptr) {
        return ok();
    }
    if (Status set = set_occlusion_view(*corner, Vec2{0.0F, 0.0F}); !set) {
        return set;
    }
    view.ambient_occlusion = corner->occlusion->import_target(graph);
    sinks.ambient_occlusion = corner->occlusion->stage();
    return ok();
}

/// Every material white and rough, and each instance's tint its albedo.
void whiten(Corner& corner, rendering::pipeline::FrameUpload& upload) noexcept {
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
        const Vec3 albedo = albedo_of(box, corner.red_wall);
        corner.instances[box].tint[0] = albedo.x;
        corner.instances[box].tint[1] = albedo.y;
        corner.instances[box].tint[2] = albedo.z;
    }
    upload.instances = Span<const rendering::pipeline::InstanceTransform>(corner.instances.data(),
                                                                          corner.instances.size());
}

Status before_upload(rendering::pipeline::FrameUpload& upload, void* user) noexcept {
    auto* corner = static_cast<Corner*>(user);
    whiten(*corner, upload);
    upload.globals.exposure_stops += kExposureShift;
    corner->flat = Vec3{upload.view.ambient_and_occlusion[0], upload.view.ambient_and_occlusion[1],
                        upload.view.ambient_and_occlusion[2]};
    rendering::pipeline::MaterialTextureSlot slots[2];
    u32 bound_slots = 0;
    if (corner->occlusion != nullptr) {
        // The jitter is known once the frame is assembled; `render.ambient_occlusion` says why the
        // view is set again here.
        const Vec2 jitter{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]};
        if (Status set = set_occlusion_view(*corner, jitter); !set) {
            return set;
        }
        slots[bound_slots++] = {kOcclusionSlot, corner->occlusion->target_view()};
        rendering::occlusion::write_occlusion_control(
            kOcclusionSlot, corner->occlusion->settings().shared, upload.view.occlusion_control);
    }
    if (corner->attach) {
        slots[bound_slots++] = corner->texture->slot(kVolumeSlot);
    }
    if (bound_slots == 0) {
        return ok();
    }
    if (Status bound = corner->scene->set_frame_textures(
            Span<const rendering::pipeline::MaterialTextureSlot>(slots, bound_slots));
        !bound) {
        return bound;
    }
    if (!corner->attach) {
        return ok();
    }
    if (corner->volume == Volume::Enabled) {
        // The camera is the origin of this scene's world, so camera-relative IS world.
        light_probes::write_probe_volume(kVolumeSlot, *corner->probes, corner->texture->layout(),
                                         Vec3{0.0F, 0.0F, 0.0F}, upload.view);
    }
    return ok();
}

gi::GiLight gi_sun() noexcept {
    gi::GiLight light;
    light.directional = true;
    light.direction = normalize(kSunTravel);
    light.colour = Vec3{1.0F, 1.0F, 1.0F};
    light.intensity = kSunLux;
    light.id = 1;
    return light;
}

/// How a run differs from the corner as described at the top of the file.
struct RunOptions {
    /// Ambient occlusion on as well.
    bool occlusion = false;
    /// False paints the red wall white.
    bool red_wall = true;
    /// False draws the frame with no light.
    bool frame_sun = true;
};

/// One scene, the volume captured from its boxes, and the frame measured.
class CornerRun {
public:
    CornerRun(DeviceFixture& fixture, Volume volume, RunOptions options = {})
        : scene_(allocator()) {
        const bool occlusion = options.occlusion;
        corner_.scene = &scene_;
        corner_.volume = volume;
        corner_.red_wall = options.red_wall;
        corner_.frame_sun = options.frame_sun;
        if (occlusion) {
            rendering::occlusion::AmbientOcclusionPassDescription description;
            description.width = kWidth;
            description.height = kHeight;
            occlusion_ready_ =
                occlusion_.create(allocator(), fixture.device(), description).has_value();
            rendering::occlusion::GtaoSettings settings;
            settings.shared.radius = 0.5F;
            occlusion_.set_settings(settings);
            corner_.occlusion = &occlusion_;
        }
        corner_.texture = &texture_;
        corner_.probes = &probes_;
        corner_.sun.kind = render::LightKind::Directional;
        corner_.sun.intensity = kSunLux;
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
            std::fprintf(stderr, "light probes: build failed: %s\n", built.error().message);
        }
        ready_ = built.has_value() && (occlusion_ready_ || !occlusion);
        scene_.set_read_back(true);
        texture_.initialize(fixture.device(), allocator());
    }

    [[nodiscard]] bool render() {
        if (!ready_) {
            return false;
        }
        rendering::assembly::AssemblyReport report{};
        // THE FIRST FRAME: the flat ambient the capture's sky is set to is read off its upload.
        if (!render_once(report)) {
            return false;
        }
        if (corner_.volume != Volume::Absent) {
            if (!capture() || !texture_.upload(probes_).has_value()) {
                return false;
            }
            corner_.attach = true;
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
    [[nodiscard]] const gi::IrradianceVolume& probes() const { return probes_; }
    [[nodiscard]] Vec3 flat() const { return corner_.flat; }

private:
    [[nodiscard]] bool render_once(rendering::assembly::AssemblyReport& report) {
        const Status rendered = scene_.render(RecordMode::Callbacks, report);
        if (!rendered) {
            std::fprintf(stderr, "light probes: render failed: %s\n", rendered.error().message);
        }
        return rendered.has_value();
    }

    [[nodiscard]] bool capture() {
        const Span<const Aabb> boxes = scene_.boxes();
        proxies_.clear();
        for (u32 box = 0; box < kUsedBoxes && box < boxes.size(); ++box) {
            if (!proxies_.add(gi::ProxyBox{boxes[box], albedo_of(box, corner_.red_wall), Vec3{}})
                     .has_value()) {
                return false;
            }
        }
        light_ = gi_sun();
        proxies_.set_lights(Span<const gi::GiLight>(&light_, 1));
        gi::IrradianceVolumeSettings settings;
        settings.origin = Vec3{-2.05F, kFloorTop + 0.3F, -8.7F};
        settings.spacing_metres = 0.75F;
        // Every surface the camera sees, and one probe beyond: past the last probe the volume
        // fades to the flat ambient over a spacing, and that edge is not what this scene is of.
        settings.count_x = 14;
        settings.count_y = 8;
        settings.count_z = 9;
        if (!probes_.configure(settings).has_value()) {
            return false;
        }
        gi::VolumeCaptureContext context;
        context.tracer = &proxies_;
        context.radiance = &proxies_;
        context.sky.zenith = corner_.flat;
        context.sky.horizon = corner_.flat;
        context.sky.ground = corner_.flat;
        // Two passes, the second with the first fed back into the surfaces it lights: one bounce,
        // then two.
        (void)probes_.capture_all(context);
        proxies_.set_indirect(&probes_);
        (void)probes_.capture_all(context);
        return true;
    }

    FrameScene scene_;
    rendering::occlusion::AmbientOcclusionPass occlusion_;
    bool occlusion_ready_ = false;
    Corner corner_;
    light_probes::ProbeVolumeTexture texture_;
    gi::IrradianceVolume probes_;
    gi::BoxProxyScene proxies_;
    gi::GiLight light_{};
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

/// What a pixel sees, as the picture's measurements need it.
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

/// The outward normal of the face of `box` that `point` lies on.
[[nodiscard]] Vec3 face_normal(const Aabb& box, Vec3 point) noexcept {
    const f32 distances[6] = {std::fabs(point.x - box.max.x), std::fabs(point.x - box.min.x),
                              std::fabs(point.y - box.max.y), std::fabs(point.y - box.min.y),
                              std::fabs(point.z - box.max.z), std::fabs(point.z - box.min.z)};
    static constexpr Vec3 kNormals[6] = {Vec3{1.0F, 0.0F, 0.0F}, Vec3{-1.0F, 0.0F, 0.0F},
                                         Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F},
                                         Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.0F, 0.0F, -1.0F}};
    u32 nearest = 0;
    for (u32 face = 1; face < 6U; ++face) {
        nearest = distances[face] < distances[nearest] ? face : nearest;
    }
    return kNormals[nearest];
}

/// Every pixel's classification, and whether its whole 3x3 neighbourhood is the same surface of the
/// same box — so an edge, where the rasteriser and this ray cast may disagree by a pixel, is never
/// measured.
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

/// Summed red and green over a region of the picture.
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
    [[nodiscard]] f64 brightness() const noexcept {
        return count > 0 ? (red + green + blue) / count : 0.0;
    }
};

/// The four regions (a) compares: the back wall within a metre of the red wall and beyond x = 2.5,
/// and the fronts of the near and far cubes.
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

// --- The committed reference --------------------------------------------------------------------

/// `references/light_probes_absent.png`: case (d)'s absent frame, rendered by the frame shader as
/// it was before the volume — the SPIR-V of 0f1dfd1 substituted for `frame_spirv.h` and the suite
/// run once with `CY_RENDER_UPDATE_GOLDEN=1`. The volume only appended fields to the frame block,
/// so the old shader reads the same frame data.
const char* before_reference_path() noexcept {
    static char storage[1024];
    (void)std::snprintf(storage, sizeof(storage), "%s/references/light_probes_absent.png",
                        CY_LIGHT_PROBES_TEST_DIR);
    return storage;
}

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

void check_against_before(const std::vector<u32>& pixels) {
    render_test::Image rendered(allocator());
    CY_REQUIRE(
        render_test::adopt(rendered, Span<const u32>(pixels.data(), pixels.size()), kWidth, kHeight)
            .has_value());
    if (updating_references()) {
        CY_CHECK(render_test::write_png(before_reference_path(), rendered).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     before_reference_path());
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    render_test::Image reference(allocator());
    const Status read = render_test::read_png(before_reference_path(), reference);
    if (!read) {
        std::fprintf(stderr, "(d) %s: %s\n", before_reference_path(), read.error().message);
    }
    CY_REQUIRE(read.has_value());
    const render_test::Comparison comparison = render_test::compare(reference, rendered);
    std::fprintf(stderr,
                 "(d) against the frame before the volume: %u differing (%u off edge), worst "
                 "delta %u at (%u, %u)\n",
                 comparison.differing, comparison.differing_off_edge, comparison.max_channel_delta,
                 comparison.worst_x, comparison.worst_y);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing, 0U);
}

}  // namespace

CY_TEST_CASE("(a) a red wall bleeds onto the white surfaces near it, not onto those far away") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    CornerRun off(fixture, Volume::Absent);
    CornerRun on(fixture, Volume::Enabled);
    // THE CONTROL: the same corner with the red wall painted white. The bounce of a sunlit white
    // floor and wall shifts every unlit surface away from the blue sky's colour on its own; what
    // the RED wall contributes is the difference between these two frames.
    RunOptions white_wall;
    white_wall.red_wall = false;
    CornerRun white(fixture, Volume::Enabled, white_wall);
    CY_REQUIRE(off.render());
    CY_REQUIRE(on.render());
    CY_REQUIRE(white.render());
    save("light-probes-off.png", off.pixels());
    save("light-probes-on.png", on.pixels());

    const Picture picture = classify_all(on.scene());
    const Bleed flat = measure_bleed(picture, off.pixels());
    const Bleed red = measure_bleed(picture, on.pixels());
    const Bleed control = measure_bleed(picture, white.pixels());
    const f64 wall_near = red.wall_near.redness() - control.wall_near.redness();
    const f64 wall_far = red.wall_far.redness() - control.wall_far.redness();
    const f64 cube_near = red.cube_near.redness() - control.cube_near.redness();
    const f64 cube_far = red.cube_far.redness() - control.cube_far.redness();
    std::fprintf(stderr,
                 "(a) redness (red/green): off, near %.3f far %.3f; on with a white wall, near "
                 "%.3f far %.3f; on with the red wall, near %.3f far %.3f. The red wall's share: "
                 "back wall near %.3f far %.3f (%u, %u px), cube near %.3f far %.3f (%u, %u px)\n",
                 flat.wall_near.redness(), flat.wall_far.redness(), control.wall_near.redness(),
                 control.wall_far.redness(), red.wall_near.redness(), red.wall_far.redness(),
                 wall_near, wall_far, red.wall_near.count, red.wall_far.count, cube_near, cube_far,
                 red.cube_near.count, red.cube_far.count);
    CY_REQUIRE(red.wall_near.count > 200U);
    CY_REQUIRE(red.wall_far.count > 200U);
    CY_REQUIRE(red.cube_near.count > 100U);
    CY_REQUIRE(red.cube_far.count > 100U);
    // OFF: every one of these surfaces shows the flat ambient on a white albedo, so the near and
    // far regions are one colour.
    CY_CHECK_LT(std::fabs(flat.wall_near.redness() - flat.wall_far.redness()), 0.01);
    CY_CHECK_LT(std::fabs(flat.cube_near.redness() - flat.cube_far.redness()), 0.01);
    // ON: the red wall tints what is near it, measurably, and what is far from it by a fraction of
    // that. MEASURED: the red wall's share of the redness is 0.424 on the back wall beside it and
    // 0.047 across the room; 0.276 on the near cube's front and 0.038 on the far one's.
    CY_CHECK_GT(wall_near, 0.2);
    CY_CHECK_GT(cube_near, 0.2);
    CY_CHECK_LT(wall_far, 0.2 * wall_near);
    CY_CHECK_LT(cube_far, 0.25 * cube_near);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) an unlit wall is lit by bounce, where the flat ambient is uniform") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    CornerRun off(fixture, Volume::Absent);
    CornerRun on(fixture, Volume::Enabled);
    CY_REQUIRE(off.render());
    CY_REQUIRE(on.render());
    const Picture picture = classify_all(on.scene());

    i32 off_low = 765;
    i32 off_high = 0;
    Tally foot;
    Tally head;
    Tally all;
    for (usize index = 0; index < picture.pixels.size(); ++index) {
        const Pixel& pixel = picture.pixels[index];
        if (!picture.interior[index] || pixel.surface != Surface::BackWall) {
            continue;
        }
        const u32 before = off.pixels()[index];
        const i32 sum = channel(before, 0) + channel(before, 1) + channel(before, 2);
        off_low = std::min(off_low, sum);
        off_high = std::max(off_high, sum);
        all.add(on.pixels()[index]);
        if (pixel.point.y < kFloorTop + 0.5F) {
            foot.add(on.pixels()[index]);
        } else if (pixel.point.y > kFloorTop + 2.5F) {
            head.add(on.pixels()[index]);
        }
    }
    std::fprintf(stderr,
                 "(b) unlit back wall: off spans %d..%d (of 765); on, foot %.1f (%u px), head "
                 "%.1f (%u px), mean %.1f\n",
                 off_low, off_high, foot.brightness(), foot.count, head.brightness(), head.count,
                 all.brightness());
    CY_REQUIRE(foot.count > 100U);
    CY_REQUIRE(head.count > 100U);
    // OFF: one colour, to within one 8-bit step per channel.
    CY_CHECK_LE(off_high - off_low, 3);
    // ON: brightest at its foot, where the sunlit floor is most of what it sees, and the spread
    // is many steps rather than one. MEASURED: off, every pixel sums to 254 of 765; on, the foot
    // averages 351.4 and the wall above 2.5 m 304.2.
    CY_CHECK_GT(foot.brightness(), head.brightness() + 6.0);
    CY_CHECK_GT(all.brightness(), static_cast<f64>(off_low));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) the frame's term follows the host volume's, pixel for pixel") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // AMBIENT ONLY: the volume captured under the sun, the frame drawn with no light at all, so
    // every pixel of every surface — the floor facing +y, the red wall's face toward +x, the back
    // wall and the cubes facing +z — is albedo times the volume's term, and a host prediction of
    // it needs no BRDF.
    RunOptions ambient_only;
    ambient_only.frame_sun = false;
    CornerRun on(fixture, Volume::Enabled, ambient_only);
    CY_REQUIRE(on.render());
    const Picture picture = classify_all(on.scene());
    const Span<const Aabb> boxes = on.scene().boxes();
    std::vector<f64> host_brightness;
    std::vector<f64> device_brightness;
    std::vector<f64> host_redness;
    std::vector<f64> device_redness;
    u32 facing[3] = {};
    for (usize index = 0; index < picture.pixels.size(); index += 2) {
        const Pixel& pixel = picture.pixels[index];
        if (!picture.interior[index] || pixel.surface == Surface::None || pixel.box >= kUsedBoxes) {
            continue;
        }
        const Vec3 normal = face_normal(boxes[pixel.box], pixel.point);
        facing[0] += normal.x > 0.5F ? 1U : 0U;
        facing[1] += normal.y > 0.5F ? 1U : 0U;
        facing[2] += normal.z > 0.5F ? 1U : 0U;
        const Vec3 host = cwise_mul(albedo_of(pixel.box, true),
                                    on.probes().ambient(pixel.point, normal, on.flat()));
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
                 "(c) over %zu pixels (%u facing +x, %u +y, %u +z): host/device correlation %.4f "
                 "in brightness, %.4f in redness; the flat ambient is (%.0f %.0f %.0f)\n",
                 host_brightness.size(), facing[0], facing[1], facing[2], brightness, redness,
                 static_cast<double>(on.flat().x), static_cast<double>(on.flat().y),
                 static_cast<double>(on.flat().z));
    CY_REQUIRE(facing[0] > 500U);
    CY_REQUIRE(facing[1] > 500U);
    CY_REQUIRE(facing[2] > 500U);
    // The device's term passes through the tonemap and 8 bits and the host's does not, so the two
    // are compared by correlation rather than by value. MEASURED: see the line above.
    CY_CHECK_GT(brightness, 0.97);
    CY_CHECK_GT(redness, 0.97);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) the volume switched off is the frame without it, byte for byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    CornerRun absent(fixture, Volume::Absent);
    CornerRun disabled(fixture, Volume::Disabled);
    CornerRun enabled(fixture, Volume::Enabled);
    CY_REQUIRE(absent.render());
    CY_REQUIRE(disabled.render());
    CY_REQUIRE(enabled.render());
    CY_CHECK_EQ(disabled.report().passes_declared, absent.report().passes_declared);
    CY_CHECK_EQ(disabled.report().execution.passes_recorded,
                absent.report().execution.passes_recorded);
    const usize changed = differing(absent.pixels(), disabled.pixels());
    std::fprintf(stderr,
                 "(d) %zu pixel(s) differ between the volume absent and attached-but-off; %zu "
                 "between absent and on\n",
                 changed, differing(absent.pixels(), enabled.pixels()));
    CY_CHECK_EQ(changed, usize{0});
    // The control: the volume switched ON does change the picture, or the two above would be equal
    // for want of a volume.
    CY_CHECK_GT(differing(absent.pixels(), enabled.pixels()), usize{1000});
    check_against_before(absent.pixels());
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(e) ambient occlusion multiplies the volume's term as it multiplied the flat one") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    CornerRun volume(fixture, Volume::Enabled);
    RunOptions occluded;
    occluded.occlusion = true;
    CornerRun both(fixture, Volume::Enabled, occluded);
    CY_REQUIRE(volume.render());
    CY_REQUIRE(both.render());
    save("light-probes-with-occlusion.png", both.pixels());
    const Picture picture = classify_all(both.scene());
    // THE CONTACT: the unlit back wall within 0.25 m of the floor, where the horizon search sees
    // the floor. OPEN: the same wall more than 1.5 m above the floor and 1 m clear of the red wall,
    // beyond the 0.5 m radius of everything.
    i32 contact_darker = 0;
    u32 contacts = 0;
    i32 open_worst = 0;
    u32 open = 0;
    for (usize index = 0; index < picture.pixels.size(); ++index) {
        const Pixel& pixel = picture.pixels[index];
        if (!picture.interior[index] || pixel.surface != Surface::BackWall) {
            continue;
        }
        const u32 a = volume.pixels()[index];
        const u32 b = both.pixels()[index];
        const f32 height = pixel.point.y - kFloorTop;
        if (height < 0.25F) {
            contact_darker += (channel(a, 0) + channel(a, 1) + channel(a, 2)) -
                              (channel(b, 0) + channel(b, 1) + channel(b, 2));
            ++contacts;
        } else if (height > 1.5F && pixel.point.x > kRedWall + 1.0F) {
            for (u32 which = 0; which < 3U; ++which) {
                open_worst = std::max(open_worst, std::abs(channel(a, which) - channel(b, which)));
            }
            ++open;
        }
    }
    std::fprintf(stderr,
                 "(e) back wall with the volume, AO off -> on: %u contact pixels darker by %.2f "
                 "steps (of 765) on average; %u open pixels, worst channel change %d\n",
                 contacts, contacts > 0 ? static_cast<double>(contact_darker) / contacts : 0.0,
                 open, open_worst);
    CY_REQUIRE(contacts > 100U);
    CY_REQUIRE(open > 1000U);
    // MEASURED: 11.27 steps darker at the contact, and one step at most in the open, where the term
    // is one and the volume's ambient passes through unchanged.
    CY_CHECK_GT(contact_darker, static_cast<i32>(contacts) * 3);
    CY_CHECK_LE(open_worst, 1);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("an unchanged volume is not uploaded again") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    gi::IrradianceVolume volume;
    gi::IrradianceVolumeSettings settings;
    settings.count_x = 2;
    settings.count_y = 2;
    settings.count_z = 2;
    CY_REQUIRE(volume.configure(settings).has_value());
    gi::VolumeCaptureContext context;
    (void)volume.capture_all(context);
    light_probes::ProbeVolumeTexture texture;
    texture.initialize(fixture.device(), allocator());
    CY_REQUIRE(texture.upload(volume).has_value());
    CY_REQUIRE(texture.upload(volume).has_value());
    CY_CHECK_EQ(texture.uploads(), 1U);
    CY_CHECK(texture.ready());
    (void)volume.invalidate(Aabb::from_min_max(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.1F, 0.1F, 0.1F}));
    (void)volume.update(context);
    CY_REQUIRE(texture.upload(volume).has_value());
    CY_CHECK_EQ(texture.uploads(), 2U);
    texture.shutdown();
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("half floats round to nearest even") {
    CY_CHECK_EQ(light_probes::half_from_float(0.0F), 0x0000U);
    CY_CHECK_EQ(light_probes::half_from_float(1.0F), 0x3C00U);
    CY_CHECK_EQ(light_probes::half_from_float(-2.0F), 0xC000U);
    CY_CHECK_EQ(light_probes::half_from_float(65504.0F), 0x7BFFU);
    CY_CHECK_EQ(light_probes::half_from_float(1.0e6F), 0x7C00U);
    CY_CHECK_EQ(light_probes::half_from_float(5.9604645e-8F), 0x0001U);
    // 1 + 2^-11 is exactly halfway between 1 and the next half; it rounds to the even one, 1.
    CY_CHECK_EQ(light_probes::half_from_float(1.0F + 0.00048828125F), 0x3C00U);
    CY_CHECK_EQ(light_probes::half_from_float(1.0F + (3.0F * 0.00048828125F)), 0x3C02U);
}
