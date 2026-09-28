// SPDX-License-Identifier: MIT
// Volumetric fog on a Vulkan device, with validation and synchronisation validation on.
// `render.volumetric_fog`.
//
// ================================================================================================
// THE SCENE, AND WHAT EACH CASE CAN FAIL ON
// ================================================================================================
//
// `pipeline_test::FrameScene` — the one scene the pipeline suites render — given a directional
// shadow map through its recorder, the fog stage through `fog::FogPass`, and two boxes where the
// cases want them: one resting on the floor and one of the same size hanging 1.6 m above it, whose
// shadow crosses open air in front of the camera. Every other box of the ring is moved out of the
// view and out of the shadow volume. The sun is turned round as `render.soft_shadows` turns it —
// down, to the right and toward the camera — so the shadow volumes lie between the boxes and the
// eye. Six properties:
//
//   (a) with the fog off the frame is BYTE-IDENTICAL to a committed reference drawn by the frame
//       shader as it was before this change;
//   (b) with the fog on and the medium EMPTY the frame is byte-identical to the frame with it off,
//       and the volume the march wrote is exactly one and exactly zero;
//   (c) LIGHT SHAFTS: in a homogeneous fog lit by the sun alone, the froxels the geometry puts in a
//       box's shadow scatter no sunlight and the ones it leaves in the open scatter all of it — the
//       per-froxel shadow sampling the requirement names, read out of the volume;
//   (d) ENERGY: the device's transmittance and in-scattering, over a height fog and a glowing,
//       forward-scattering sphere, are the single-scattering integral taken by brute force on the
//       processor, and the host march `integrate_fog_column` to within float rounding;
//   (e) the frame applies it: through an absorbing fog every floor pixel is no brighter than it
//       was, and the far floor loses more than the near floor;
//   (f) the atmosphere-table variant, over an air table the processor built, is the host march
//       that composites the same air — the device half of "composited consistently with fog".

#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/fog/fog_pass.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/sky/atmosphere.h>
#include <cy/rendering/sky/tables.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

using namespace cy;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using cy::rendering::fog::FogAtPoint;
using cy::rendering::fog::FogFrame;
using cy::rendering::fog::FogMedium;
using cy::rendering::fog::FogPass;
using cy::rendering::fog::FogPassDescription;
using cy::rendering::fog::FogSettings;
using cy::rendering::fog::FogTextureExtent;
using cy::rendering::fog::FogView;
using cy::rendering::fog::FogVolumeShape;
using cy::rendering::fog::HostShadowMap;

namespace {

/// The frame's set 0 texture-table slots the shadow map and the fog volume are bound at. Any free
/// slots; the scene binds no material texture of its own.
constexpr u32 kShadowSlot = 121;
constexpr u32 kFogSlot = 123;
/// The shadow volume: `render.soft_shadows`' orthographic box, 18 m across, 8.8 mm a texel.
constexpr f32 kShadowRadius = 9.0F;
constexpr f32 kShadowNear = 0.1F;
constexpr f32 kShadowFar = kShadowRadius * 4.0F;
const Vec3 kShadowCentre{0.0F, -1.8F, -7.0F};
constexpr u32 kShadowExtent = 2048;
/// The frame's own depth bias (`directionalShadowVisibility`), so fog and surfaces agree on what
/// the map says.
constexpr f32 kShadowBias = 0.0005F;

constexpr f32 kFloorTop = -1.9F + 0.125F;
constexpr f32 kHalf = 0.45F;
constexpr f32 kHang = 1.6F;
const Vec3 kRestingCentre{-1.6F, kFloorTop + kHalf, -5.8F};
const Vec3 kHangingCentre{1.3F, kFloorTop + kHalf + kHang, -6.6F};

/// The volume: a quarter of the frame's pixels across, 40 slices out to 24 m — past the floor's
/// far edge at 14 m.
[[nodiscard]] FogSettings fog_settings() noexcept {
    FogSettings settings;
    settings.volume.width = 60;
    settings.volume.height = 34;
    settings.volume.depth = 40;
    settings.volume.near_plane = 0.1F;
    settings.volume.far_plane = 24.0F;
    settings.volume.depth_exponent = 2.0F;
    settings.steps_per_slice = 4;
    return settings;
}

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
        description.application_name = "cy_test_render_volumetric_fog";
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

/// `references/volumetric_fog_off.png`: (a)'s frame — the shadow map on, the fog off — rendered by
/// the frame shader as it was before this change: the suite run once with
/// `CY_RENDER_UPDATE_GOLDEN=1` against main's `frame_spirv.h`, before the shader was regenerated.
/// The change only appended `volumetricFogControl` to the frame block, so the old shader reads the
/// same frame data.
const char* before_reference_path() noexcept {
    static char storage[1024];
    (void)std::snprintf(storage, sizeof(storage), "%s/references/volumetric_fog_off.png",
                        CY_FOG_TEST_DIR);
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
        std::fprintf(stderr, "(a) %s: %s\n", before_reference_path(), read.error().message);
    }
    CY_REQUIRE(read.has_value());
    const render_test::Comparison comparison = render_test::compare(reference, rendered);
    std::fprintf(stderr,
                 "(a) against the frame before the change: %u differing (%u off edge), worst "
                 "delta %u at (%u, %u)\n",
                 comparison.differing, comparison.differing_off_edge, comparison.max_channel_delta,
                 comparison.worst_x, comparison.worst_y);
    CY_CHECK(comparison.comparable);
    // BYTE FOR BYTE: `compare` forgives one 8-bit step in `differing`, so the largest channel
    // difference is what says the two frames are the same bytes.
    CY_CHECK_EQ(comparison.differing, 0U);
    CY_CHECK_EQ(comparison.max_channel_delta, 0U);
}

// --- The frame ---------------------------------------------------------------------------------

/// The way the sun travels in these cases: `render.soft_shadows`' direction, so the shadow volumes
/// lie between the boxes and the camera.
[[nodiscard]] Vec3 sun_travel() noexcept {
    const Vec3 travel{0.45F, -0.72F, 0.55F};
    return travel * (1.0F / std::sqrt(dot(travel, travel)));
}

[[nodiscard]] Mat4 shadow_to_clip() noexcept {
    const Vec3 travel = sun_travel();
    const Vec3 eye = kShadowCentre - (travel * (kShadowRadius * 2.0F));
    const Mat4 view = look_at(eye, kShadowCentre, Vec3{0.0F, 1.0F, 0.0F});
    const Mat4 projection = orthographic_reversed_z(-kShadowRadius, kShadowRadius, -kShadowRadius,
                                                    kShadowRadius, kShadowNear, kShadowFar);
    return projection * view;
}

/// The march's view of the frame: the scene's camera at the origin of its relative space.
[[nodiscard]] FogView fog_view(FrameScene& scene) noexcept {
    return cy::rendering::fog::fog_view_from(scene.view(), 0.9F,
                                             static_cast<f32>(kWidth) / static_cast<f32>(kHeight),
                                             Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, 0.0F});
}

/// What a frame case configures.
struct FogCase {
    FrameScene* scene = nullptr;
    FogPass* pass = nullptr;
    rhi::TextureHandle shadow_color;
    rhi::TextureHandle shadow_depth;
    rhi::TextureViewHandle shadow_view;
    /// The fog stage declared and its volume written.
    bool attached = false;
    /// The frame's control word names the volume, so the opaque pass reads it.
    bool applied = false;
    /// The march reads the shadow map; off, every froxel is lit.
    bool shadowed_fog = true;
    /// The medium lit by the frame's ambient term as well as the sun.
    bool fog_ambient = true;
    FogFrame fog{};
    rendering::FrameResourceRead reads[1] = {};
    std::vector<rendering::GpuLight> lights;
};

void configure_case(rendering::assembly::AssemblyDescription& description, void* user) noexcept {
    const auto* frame = static_cast<const FogCase*>(user);
    description.pin_jitter = true;
    description.post.volumetric_fog = frame->attached;
}

/// Two boxes where the cases want them, and the rest of the ring out of the view and out of the
/// shadow volume.
void place_box(u32 which, Vec3& centre, f32& half, void* /*user*/) noexcept {
    switch (which) {
        case 1:
            centre = kRestingCentre;
            half = kHalf;
            return;
        case 2:
            centre = kHangingCentre;
            half = kHalf;
            return;
        default:
            centre = Vec3{static_cast<f32>(which) * 3.0F, 0.0F, 60.0F};
            half = 0.3F;
            return;
    }
}

Status before_assemble(rendering::RenderGraph& graph, rendering::assembly::AssemblyView& view,
                       rendering::assembly::FrameSinks& sinks, void* user) noexcept {
    auto* frame = static_cast<FogCase*>(user);
    rendering::TextureRequest request;
    request.name = "fog test shadow map";
    request.format = rhi::Format::R32Sfloat;
    request.width = kShadowExtent;
    request.height = kShadowExtent;
    view.shadow_color =
        graph.import_texture(request, frame->shadow_color, rhi::ImageUse::Undefined);
    request.name = "fog test shadow depth";
    request.format = rhi::Format::D32Sfloat;
    view.shadow_depth =
        graph.import_texture(request, frame->shadow_depth, rhi::ImageUse::Undefined);
    rendering::pipeline::FrameRecorder& recorder = frame->scene->recorder();
    recorder.set_shadow_targets(view.shadow_color, view.shadow_depth, kShadowExtent);
    const auto shadow = static_cast<usize>(rendering::FramePassKind::Shadow);
    sinks.passes[shadow] = recorder.sinks().passes[shadow];
    // The opaque pass samples the map through the texture table, so it declares the read that
    // makes the graph transition it. The fog volume's read the frame declares itself.
    const auto opaque = static_cast<usize>(rendering::FramePassKind::Opaque);
    frame->reads[0] =
        rendering::FrameResourceRead{view.shadow_color, rhi::Access::FragmentSampledRead};
    sinks.passes[opaque].reads = Span<const rendering::FrameResourceRead>(frame->reads, 1);

    if (!frame->attached) {
        return ok();
    }
    frame->fog.view = fog_view(*frame->scene);
    frame->fog.shadow.enabled = frame->shadowed_fog;
    frame->fog.shadow.relative_to_uv = cy::rendering::fog::shadow_uv_rows(shadow_to_clip(), true);
    frame->fog.shadow.bias = kShadowBias;
    frame->fog.shadow.extent = kShadowExtent;
    frame->fog.shadow_resource = view.shadow_color;
    if (Status set = frame->pass->set_frame(frame->fog); !set) {
        return set;
    }
    view.volumetric_fog = frame->pass->import_target(graph);
    sinks.volumetric_fog = frame->pass->stage();
    return ok();
}

[[nodiscard]] u32 directional_index(Span<const rendering::GpuLight> lights) noexcept {
    for (usize index = 0; index < lights.size(); ++index) {
        if (lights[index].kind == rendering::kGpuLightDirectional) {
            return static_cast<u32>(index);
        }
    }
    return static_cast<u32>(lights.size());
}

Status before_upload(rendering::pipeline::FrameUpload& upload, void* user) noexcept {
    auto* frame = static_cast<FogCase*>(user);
    const u32 sun = directional_index(upload.lights);
    frame->lights.assign(upload.lights.begin(), upload.lights.end());
    if (sun < frame->lights.size()) {
        const Vec3 travel = sun_travel();
        rendering::GpuLight& light = frame->lights[sun];
        light.direction[0] = travel.x;
        light.direction[1] = travel.y;
        light.direction[2] = travel.z;
        // THE FOG IS LIT BY THE LIGHT THE SURFACES ARE: the sun's illuminance as the frame
        // uploads it, and the frame's own ambient radiance.
        frame->fog.light.to_sun = travel * -1.0F;
        frame->fog.light.sun_illuminance =
            Vec3{light.color[0], light.color[1], light.color[2]} * light.intensity;
    }
    upload.lights = Span<const rendering::GpuLight>(frame->lights.data(), frame->lights.size());
    frame->fog.light.ambient_radiance =
        frame->fog_ambient
            ? Vec3{upload.view.ambient_and_occlusion[0], upload.view.ambient_and_occlusion[1],
                   upload.view.ambient_and_occlusion[2]}
            : Vec3{0.0F, 0.0F, 0.0F};
    if (frame->attached) {
        if (Status set = frame->pass->set_frame(frame->fog); !set) {
            return set;
        }
    }
    const Mat4 to_clip = shadow_to_clip();
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            upload.view.shadow_to_clip[(row * 4U) + column] = to_clip.at(row, column);
        }
    }
    upload.view.shadow_control[0] = kShadowSlot;
    upload.view.shadow_control[1] = sun;
    upload.view.shadow_control[2] = kShadowExtent;
    upload.view.shadow_control[3] = 1U;
    if (frame->applied) {
        upload.view.volumetric_fog_control[0] = kFogSlot;
    }

    rendering::pipeline::MaterialTextureSlot slots[2] = {
        {kShadowSlot, frame->shadow_view},
        {kFogSlot, frame->attached ? frame->pass->target_view() : rhi::TextureViewHandle{}},
    };
    return frame->scene->set_frame_textures(
        Span<const rendering::pipeline::MaterialTextureSlot>(slots, frame->attached ? 2U : 1U));
}

/// One scene, built with a case's hooks, and the frame it rendered.
class FrameRun {
public:
    struct Options {
        bool attached = false;
        bool applied = false;
        bool shadowed_fog = true;
        bool fog_ambient = true;
        FogMedium medium{};
    };

    FrameRun(DeviceFixture& fixture, const Options& options)
        : device_(&fixture.device()), scene_(allocator()) {
        frame_.scene = &scene_;
        frame_.pass = &pass_;
        frame_.attached = options.attached;
        frame_.applied = options.applied;
        frame_.shadowed_fog = options.shadowed_fog;
        frame_.fog_ambient = options.fog_ambient;
        ready_ = create_shadow_map();
        if (options.attached) {
            FogPassDescription description;
            description.settings = fog_settings();
            description.readback = true;
            ready_ = ready_ && pass_.create(allocator(), fixture.device(), description).has_value();
            pass_.set_medium(options.medium);
        }
        FrameSceneHooks hooks;
        hooks.user = &frame_;
        hooks.configure = &configure_case;
        hooks.before_assemble = &before_assemble;
        hooks.before_upload = &before_upload;
        hooks.place_box = &place_box;
        scene_.set_hooks(hooks);
        const Status built = scene_.build(fixture.device());
        if (!built) {
            std::fprintf(stderr, "fog frame: build failed: %s\n", built.error().message);
        }
        ready_ = ready_ && built.has_value();
        scene_.set_read_back(true);
    }

    ~FrameRun() {
        (void)device_->wait_idle();
        pass_.destroy();
        scene_.release();
        if (!frame_.shadow_view.is_null()) {
            device_->destroy_texture_view(frame_.shadow_view);
        }
        for (rhi::TextureHandle* texture : {&frame_.shadow_color, &frame_.shadow_depth}) {
            if (!texture->is_null()) {
                device_->destroy_texture(*texture);
            }
        }
    }
    FrameRun(const FrameRun&) = delete;
    FrameRun& operator=(const FrameRun&) = delete;

    [[nodiscard]] bool render() {
        if (!ready_) {
            return false;
        }
        const Status rendered = scene_.render(RecordMode::Callbacks, report_);
        if (!rendered) {
            std::fprintf(stderr, "fog frame: render failed: %s\n", rendered.error().message);
            return false;
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        if (frame_.attached) {
            const FogTextureExtent extent = pass_.extent();
            volume_.assign(static_cast<usize>(extent.width) * extent.height, Vec4{});
            return pass_.read_back(Span<Vec4>(volume_.data(), volume_.size())).has_value();
        }
        return true;
    }

    [[nodiscard]] const std::vector<u32>& pixels() const { return pixels_; }
    [[nodiscard]] const std::vector<Vec4>& volume() const { return volume_; }
    [[nodiscard]] FrameScene& scene() { return scene_; }
    [[nodiscard]] const FogFrame& fog() const { return frame_.fog; }
    [[nodiscard]] const FogPass& pass() const { return pass_; }

private:
    [[nodiscard]] bool create_shadow_map() {
        rhi::TextureDescription texture;
        texture.name = "fog test shadow map";
        texture.format = rhi::Format::R32Sfloat;
        texture.extent = rhi::Extent3D{kShadowExtent, kShadowExtent, 1};
        texture.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        auto color = device_->create_texture(texture);
        if (!color.has_value()) {
            return false;
        }
        frame_.shadow_color = *color;
        texture.name = "fog test shadow depth";
        texture.format = rhi::Format::D32Sfloat;
        texture.usage = rhi::TextureUsage::DepthStencilAttachment;
        auto depth = device_->create_texture(texture);
        if (!depth.has_value()) {
            return false;
        }
        frame_.shadow_depth = *depth;
        rhi::TextureViewDescription view;
        view.name = "fog test shadow map";
        view.texture = frame_.shadow_color;
        auto made = device_->create_texture_view(view);
        if (!made.has_value()) {
            return false;
        }
        frame_.shadow_view = *made;
        return true;
    }

    rhi::Device* device_ = nullptr;
    FrameScene scene_;
    FogPass pass_;
    FogCase frame_;
    rendering::assembly::AssemblyReport report_{};
    std::vector<u32> pixels_;
    std::vector<Vec4> volume_;
    bool ready_ = false;
};

// --- Reading the volume back ---------------------------------------------------------------------

struct Froxel {
    Vec3 transmittance{1.0F, 1.0F, 1.0F};
    Vec3 in_scattering{0.0F, 0.0F, 0.0F};
};

[[nodiscard]] Froxel froxel_at(const std::vector<Vec4>& texels, u32 x, u32 y, u32 slice) noexcept {
    const FogSettings settings = fog_settings();
    const FogTextureExtent extent = cy::rendering::fog::fog_texture_extent(settings.volume);
    const usize row = 1U + (static_cast<usize>(slice) * settings.volume.height) + y;
    Froxel froxel;
    froxel.transmittance = texels[(row * extent.width) + x].xyz();
    froxel.in_scattering = texels[(row * extent.width) + settings.volume.width + x].xyz();
    return froxel;
}

[[nodiscard]] f32 luminance(Vec3 colour) noexcept {
    return (0.2126F * colour.x) + (0.7152F * colour.y) + (0.0722F * colour.z);
}

[[nodiscard]] f32 relative_error(Vec3 got, Vec3 want) noexcept {
    const f32 scale = std::max(max_component(want), 1e-9F);
    return max_component(cwise_abs(got - want)) / scale;
}

// --- The geometry's answers ----------------------------------------------------------------------

/// The slab test: the entry distance of a ray into a box, or infinity when it misses. A ray that
/// starts inside enters at zero.
[[nodiscard]] f32 ray_aabb(Vec3 origin, Vec3 direction, const Aabb& box) noexcept {
    f32 enter = 0.0F;
    f32 leave = INFINITY;
    for (u32 axis = 0; axis < 3; ++axis) {
        const f32 o = origin[axis];
        const f32 d = direction[axis];
        if (std::fabs(d) < 1e-12F) {
            if (o < box.min[axis] || o > box.max[axis]) {
                return INFINITY;
            }
            continue;
        }
        f32 t0 = (box.min[axis] - o) / d;
        f32 t1 = (box.max[axis] - o) / d;
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
        if (enter > leave) {
            return INFINITY;
        }
    }
    return enter;
}

/// Whether the sun reaches `point` past every box in the scene, the floor slab included.
[[nodiscard]] bool sun_reaches(FrameScene& scene, Vec3 point) noexcept {
    const Vec3 to_sun = sun_travel() * -1.0F;
    for (const Aabb& box : scene.boxes()) {
        if (std::isfinite(ray_aabb(point, to_sun, box))) {
            return false;
        }
    }
    return true;
}

/// Whether the map covers `point` at all: inside its square and its depth range. Outside, the
/// march calls a point lit whatever the geometry says, as both frames' surface lookups do.
[[nodiscard]] bool map_covers(const Mat4& relative_to_uv, Vec3 point) noexcept {
    const Vec4 p{point.x, point.y, point.z, 1.0F};
    const Vec4 q{dot(relative_to_uv.row(0), p), dot(relative_to_uv.row(1), p),
                 dot(relative_to_uv.row(2), p), dot(relative_to_uv.row(3), p)};
    const f32 u = q.x / q.w;
    const f32 v = q.y / q.w;
    const f32 depth = q.z / q.w;
    return u > 0.01F && u < 0.99F && v > 0.01F && v < 0.99F && depth > 0.01F && depth < 0.99F;
}

}  // namespace

CY_TEST_CASE(
    "with the fog off the frame is the frame before it, and an empty medium changes nothing") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameRun off(fixture, FrameRun::Options{});
    CY_REQUIRE(off.render());
    save("volumetric_fog_off.png", off.pixels());
    check_against_before(off.pixels());

    // (b) The whole path on — the stage declared, the march dispatched, the volume bound and read
    // by every surface — over a medium with nothing in it.
    FrameRun::Options empty;
    empty.attached = true;
    empty.applied = true;
    FrameRun on(fixture, empty);
    CY_REQUIRE(on.render());
    save("volumetric_fog_empty.png", on.pixels());
    CY_REQUIRE_EQ(on.pixels().size(), off.pixels().size());
    usize differing = 0;
    for (usize index = 0; index < off.pixels().size(); ++index) {
        differing += on.pixels()[index] != off.pixels()[index] ? 1U : 0U;
    }
    std::fprintf(stderr, "(b) an empty medium against the frame with fog off: %zu differing\n",
                 differing);
    CY_CHECK_EQ(differing, 0U);
    const FogSettings settings = fog_settings();
    usize exact = 0;
    for (u32 slice = 0; slice < settings.volume.depth; ++slice) {
        for (u32 y = 0; y < settings.volume.height; ++y) {
            for (u32 x = 0; x < settings.volume.width; ++x) {
                const Froxel froxel = froxel_at(on.volume(), x, y, slice);
                exact += froxel.transmittance == (Vec3{1.0F, 1.0F, 1.0F}) &&
                                 froxel.in_scattering == (Vec3{0.0F, 0.0F, 0.0F})
                             ? 1U
                             : 0U;
            }
        }
    }
    CY_CHECK_EQ(exact, static_cast<usize>(settings.volume.width) * settings.volume.height *
                           settings.volume.depth);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("froxels in an occluder's shadow scatter no sunlight and the open ones all of it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // A homogeneous haze of about 80 m visibility, lit by the sun ALONE, so a froxel's local
    // in-scattering is the sun's share of it and nothing else.
    FrameRun::Options options;
    options.attached = true;
    options.applied = true;
    options.fog_ambient = false;
    options.medium.height.extinction = cy::rendering::fog::extinction_for_visibility(80.0F);
    options.medium.height.base_height = 1000.0F;
    options.medium.height.anisotropy = 0.3F;
    FrameRun run(fixture, options);
    CY_REQUIRE(run.render());
    save("volumetric_fog_shafts.png", run.pixels());

    const FogSettings settings = fog_settings();
    const FogView view = run.fog().view;
    const f32 extinction = options.medium.height.extinction;
    const Mat4& to_uv = run.fog().shadow.relative_to_uv;
    usize shadowed = 0;
    usize shadowed_dark = 0;
    usize open = 0;
    usize open_lit = 0;
    u32 best_column[2] = {0, 0};
    u32 best_run = 0;
    for (u32 y = 0; y < settings.volume.height; ++y) {
        for (u32 x = 0; x < settings.volume.width; ++x) {
            const Vec3 direction = cy::rendering::fog::fog_column_direction(settings, view, x, y);
            const f32 cosine = dot(direction, view.forward);
            const f32 phase = cy::rendering::henyey_greenstein(
                options.medium.height.anisotropy, dot(direction, run.fog().light.to_sun));
            const Vec3 source = cwise_mul(options.medium.height.albedo * (extinction * phase),
                                          run.fog().light.sun_illuminance);
            f32 previous = 0.0F;
            Froxel before;
            u32 run_length = 0;
            for (u32 slice = 0; slice < settings.volume.depth; ++slice) {
                const f32 distance =
                    cy::rendering::froxel_slice_depth(settings.volume, slice) / cosine;
                const Froxel froxel = froxel_at(run.volume(), x, y, slice);
                // The froxel's own share: what it added, seen from its near edge, against what a
                // fully lit froxel of the same medium adds.
                const f32 added = luminance(froxel.in_scattering - before.in_scattering);
                const f32 full = luminance(source) *
                                 (before.transmittance.x - froxel.transmittance.x) / extinction;
                // The geometry's answer at eight points along the froxel's stretch of the ray, and
                // the froxels either wholly in shadow or wholly in the open, with the map covering
                // them, are the ones counted.
                u32 reached = 0;
                bool covered = true;
                constexpr u32 kProbes = 8;
                for (u32 probe = 0; probe < kProbes; ++probe) {
                    const f32 along = previous + ((static_cast<f32>(probe) + 0.5F) *
                                                  (distance - previous) / kProbes);
                    const Vec3 point = direction * along;
                    covered = covered && map_covers(to_uv, point);
                    reached += sun_reaches(run.scene(), point) ? 1U : 0U;
                }
                if (covered && full > 0.0F) {
                    const f32 fraction = added / full;
                    if (reached == 0U) {
                        ++shadowed;
                        shadowed_dark += fraction < 0.1F ? 1U : 0U;
                        ++run_length;
                        if (run_length > best_run) {
                            best_run = run_length;
                            best_column[0] = x;
                            best_column[1] = y;
                        }
                    } else if (reached == kProbes) {
                        ++open;
                        open_lit += fraction > 0.9F ? 1U : 0U;
                        run_length = 0;
                    }
                }
                before = froxel;
                previous = distance;
            }
        }
    }
    std::fprintf(stderr,
                 "(c) %zu froxels wholly in a shadow, %zu of them below a tenth of the sun; %zu "
                 "in the open, %zu of them above nine tenths; longest shadowed run %u slices in "
                 "column (%u, %u)\n",
                 shadowed, shadowed_dark, open, open_lit, best_run, best_column[0], best_column[1]);
    // The case has a subject: shadow volumes crossing open air, and the open air around them.
    CY_REQUIRE_GT(shadowed, 200U);
    CY_REQUIRE_GT(open, 2000U);
    CY_CHECK_GT(static_cast<f32>(shadowed_dark), 0.97F * static_cast<f32>(shadowed));
    CY_CHECK_GT(static_cast<f32>(open_lit), 0.97F * static_cast<f32>(open));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the device's volume is the single-scattering integral over the medium") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // A ground fog pooling on the floor and thinning upward, and a glowing, forward-scattering
    // sphere over it, lit by the sun and the sky with no shadow: every term of the source and a
    // medium that varies along every ray.
    FrameRun::Options options;
    options.attached = true;
    options.applied = true;
    options.shadowed_fog = false;
    options.medium.height.extinction = 0.12F;
    options.medium.height.base_height = kFloorTop;
    options.medium.height.scale_height = 1.5F;
    options.medium.height.albedo = Vec3{0.9F, 0.92F, 0.95F};
    options.medium.height.anisotropy = 0.6F;
    options.medium.volume_count = 1;
    cy::rendering::fog::FogVolume& sphere = options.medium.volumes[0];
    sphere.shape = FogVolumeShape::Sphere;
    sphere.centre = Vec3{-0.8F, 0.2F, -9.0F};
    sphere.size = Vec3{2.2F, 0.0F, 0.0F};
    sphere.edge = 0.8F;
    sphere.extinction = 0.35F;
    sphere.albedo = Vec3{0.7F, 0.6F, 0.5F};
    sphere.anisotropy = 0.4F;
    sphere.emission = Vec3{300.0F, 120.0F, 40.0F};
    FrameRun run(fixture, options);
    CY_REQUIRE(run.render());
    save("volumetric_fog_energy.png", run.pixels());

    const FogSettings settings = fog_settings();
    const FogFrame& fog = run.fog();
    std::vector<Vec3> t(settings.volume.depth);
    std::vector<Vec3> s(settings.volume.depth);
    f32 worst_reference = 0.0F;
    f32 worst_reference_t = 0.0F;
    f32 worst_march = 0.0F;
    usize compared = 0;
    for (u32 y = 1; y < settings.volume.height; y += 4) {
        for (u32 x = 2; x < settings.volume.width; x += 5) {
            CY_REQUIRE(cy::rendering::fog::integrate_fog_column(
                           settings, fog.view, fog.light, fog.shadow, HostShadowMap{},
                           run.pass().medium(), x, y, Span<Vec3>(t), Span<Vec3>(s))
                           .has_value());
            const Vec3 direction =
                cy::rendering::fog::fog_column_direction(settings, fog.view, x, y);
            const f32 cosine = dot(direction, fog.view.forward);
            for (u32 slice = 0; slice < settings.volume.depth; ++slice) {
                const Froxel froxel = froxel_at(run.volume(), x, y, slice);
                worst_march = std::max(worst_march, relative_error(froxel.in_scattering, s[slice]));
                if (slice % 6U != 5U) {
                    continue;
                }
                const f32 distance =
                    cy::rendering::froxel_slice_depth(settings.volume, slice) / cosine;
                const FogAtPoint reference = cy::rendering::fog::single_scattering_reference(
                    fog.view, fog.light, fog.shadow, HostShadowMap{}, run.pass().medium(),
                    direction, distance, 20'000);
                worst_reference = std::max(
                    worst_reference, relative_error(froxel.in_scattering, reference.in_scattering));
                worst_reference_t =
                    std::max(worst_reference_t,
                             std::fabs(froxel.transmittance.x - reference.transmittance.x));
                ++compared;
            }
        }
    }
    std::fprintf(stderr,
                 "(d) %zu froxels against the single-scattering integral: worst in-scattering "
                 "%.3g, worst transmittance %.3g; every sampled column against the host march: "
                 "worst %.3g\n",
                 compared, static_cast<double>(worst_reference),
                 static_cast<double>(worst_reference_t), static_cast<double>(worst_march));
    CY_REQUIRE_GT(compared, 100U);
    CY_CHECK_LT(worst_reference, 1e-2F);
    CY_CHECK_LT(worst_reference_t, 2e-3F);
    CY_CHECK_LT(worst_march, 1e-3F);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("a surface seen through fog is attenuated with its distance") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameRun off(fixture, FrameRun::Options{});
    CY_REQUIRE(off.render());
    // A purely absorbing medium — albedo zero, so nothing is scattered toward the eye and the
    // transmittance is the whole of what the volume does.
    FrameRun::Options options;
    options.attached = true;
    options.applied = true;
    options.medium.height.extinction = 0.15F;
    options.medium.height.base_height = 1000.0F;
    options.medium.height.albedo = Vec3{0.0F, 0.0F, 0.0F};
    FrameRun on(fixture, options);
    CY_REQUIRE(on.render());
    save("volumetric_fog_absorbing.png", on.pixels());

    // The floor's pixels, by the distance the camera sees them at: the floor slab's top face,
    // reached before any box.
    const Mat4& projection = off.scene().projection();
    f32 near_sum = 0.0F;
    f32 far_sum = 0.0F;
    usize near_count = 0;
    usize far_count = 0;
    usize brighter = 0;
    const auto channel = [](u32 texel, u32 shift) {
        return static_cast<f32>((texel >> shift) & 0xFFU);
    };
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const f32 ndc_x = (((static_cast<f32>(x) + 0.5F) / kWidth) * 2.0F) - 1.0F;
            const f32 ndc_y = 1.0F - (((static_cast<f32>(y) + 0.5F) / kHeight) * 2.0F);
            const Vec3 ray{ndc_x / projection.columns[0].x, ndc_y / projection.columns[1].y, -1.0F};
            const Span<const Aabb> boxes = off.scene().boxes();
            const f32 floor_t = ray_aabb(Vec3{}, ray, boxes[0]);
            if (!std::isfinite(floor_t)) {
                continue;
            }
            bool hidden = false;
            for (usize index = 1; index < boxes.size(); ++index) {
                hidden = hidden || ray_aabb(Vec3{}, ray, boxes[index]) < floor_t;
            }
            const Vec3 point = ray * floor_t;
            if (hidden || std::fabs(point.y - kFloorTop) > 1e-3F) {
                continue;
            }
            const usize pixel = (static_cast<usize>(y) * kWidth) + x;
            const u32 was = off.pixels()[pixel];
            const u32 now = on.pixels()[pixel];
            const f32 was_g = channel(was, 8);
            if (was_g < 16.0F) {
                continue;  // too dark for a ratio to mean anything
            }
            for (const u32 shift : {0U, 8U, 16U}) {
                brighter += channel(now, shift) > channel(was, shift) ? 1U : 0U;
            }
            const f32 ratio = channel(now, 8) / was_g;
            const f32 distance = length(point);
            if (distance < 5.5F) {
                near_sum += ratio;
                ++near_count;
            } else if (distance > 9.0F) {
                far_sum += ratio;
                ++far_count;
            }
        }
    }
    CY_REQUIRE_GT(near_count, 500U);
    CY_REQUIRE_GT(far_count, 500U);
    const f32 near_ratio = near_sum / static_cast<f32>(near_count);
    const f32 far_ratio = far_sum / static_cast<f32>(far_count);
    std::fprintf(stderr,
                 "(e) the floor through an absorbing fog: near kept %.3f, far kept %.3f, %zu "
                 "channels brighter\n",
                 static_cast<double>(near_ratio), static_cast<double>(far_ratio), brighter);
    CY_CHECK_EQ(brighter, 0U);
    CY_CHECK_LT(far_ratio, near_ratio * 0.8F);
    CY_CHECK_LT(near_ratio, 0.999F);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("the atmosphere-table variant composites the air the processor built") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    rhi::Device& device = fixture.device();
    namespace sky = cy::rendering::sky;

    // The atmosphere's own table for a camera 120 m up looking at the horizon, built on the
    // processor exactly as `samples/10-world` builds it, and a valley haze beneath it.
    FogSettings settings;
    settings.volume.width = 24;
    settings.volume.height = 14;
    settings.volume.depth = 32;
    settings.volume.near_plane = 1.0F;
    settings.volume.far_plane = 6000.0F;
    settings.volume.depth_exponent = 2.0F;
    settings.steps_per_slice = 3;
    FogView view;
    view.eye = Vec3{0.0F, 120.0F, 0.0F};
    view.forward = normalize(Vec3{0.0F, -0.06F, -1.0F});
    view.right = normalize(cross(view.forward, Vec3{0.0F, 1.0F, 0.0F}));
    view.up = cross(view.right, view.forward);
    const Vec3 to_sun = normalize(Vec3{-0.3F, 0.25F, -0.8F});
    const sky::Atmosphere atmosphere = sky::earth_atmosphere();
    sky::AtmosphereTables tables;
    CY_REQUIRE(tables.configure(sky::SkyTableQuality::Medium).has_value());
    CY_REQUIRE(tables.build(atmosphere).has_value());
    cy::rendering::FroxelVolume air_volume = settings.volume;
    air_volume.width = 16;
    air_volume.height = 9;
    sky::AerialPerspectiveTable air(allocator());
    CY_REQUIRE(air.configure(air_volume).has_value());
    sky::AerialPerspectiveTable::View basis;
    basis.forward = view.forward;
    basis.right = view.right;
    basis.up = view.up;
    basis.tan_half_fov_x = view.tan_half_fov_x;
    basis.tan_half_fov_y = view.tan_half_fov_y;
    CY_REQUIRE(
        air.update(atmosphere, tables, sky::ground_position(atmosphere, 120.0F), basis, to_sun)
            .has_value());
    cy::Array<Vec4> words(allocator());
    CY_REQUIRE(sky::pack_aerial_perspective(air, 1.0F, words).has_value());
    rhi::BufferDescription upload;
    upload.name = "fog test atmosphere";
    upload.size = words.size() * sizeof(Vec4);
    upload.usage = rhi::BufferUsage::Storage;
    upload.memory = rhi::MemoryUse::Upload;
    auto air_buffer = device.create_buffer(upload);
    CY_REQUIRE(air_buffer.has_value());
    std::memcpy(device.buffer_mapped_pointer(*air_buffer), words.data(),
                static_cast<usize>(upload.size));

    FogMedium medium;
    medium.height.extinction = cy::rendering::fog::extinction_for_visibility(1500.0F);
    medium.height.base_height = 0.0F;
    medium.height.scale_height = 60.0F;
    FogFrame fog;
    fog.view = view;
    fog.light.to_sun = to_sun;
    fog.light.sun_illuminance = Vec3{1.0F, 0.93F, 0.8F};
    fog.light.ambient_radiance = Vec3{0.25F, 0.3F, 0.4F};

    FogPass pass;
    FogPassDescription description;
    description.settings = settings;
    description.target = cy::rendering::fog::FogTarget::AerialTable;
    description.readback = true;
    CY_REQUIRE(pass.create(allocator(), device, description).has_value());
    pass.set_medium(medium);

    std::vector<Vec4> table(pass.readback_count());
    {
        rendering::RenderGraph graph(allocator());
        rendering::BufferRequest request;
        request.name = "fog test atmosphere";
        request.size = upload.size;
        fog.air = graph.import_buffer(request, *air_buffer);
        CY_REQUIRE(pass.set_frame(fog).has_value());
        rendering::ScreenSpaceStageInputs inputs;
        inputs.target = pass.import_target(graph);
        CY_REQUIRE_NE(pass.declare(graph, inputs), rendering::kInvalidPass);
        CY_REQUIRE(graph.status().has_value());
        CY_REQUIRE(device.begin_frame().has_value());
        {
            rendering::GraphExecutor executor(allocator(), device);
            CY_REQUIRE(
                executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{})
                    .has_value());
            CY_REQUIRE(device.wait_idle().has_value());
            executor.release();
        }
        CY_REQUIRE(device.end_frame().has_value());
        CY_REQUIRE(pass.read_back(Span<Vec4>(table.data(), table.size())).has_value());
    }

    // The header is the atmosphere table's: the basis, the shape and the planes, switched on.
    CY_CHECK_EQ(table[0].w, 1.0F);
    CY_CHECK_EQ(table[3].x, static_cast<f32>(settings.volume.width));
    CY_CHECK_EQ(table[3].z, static_cast<f32>(settings.volume.depth));
    CY_CHECK_EQ(table[4].y, settings.volume.far_plane);
    // And every froxel is the host march over the same air.
    std::vector<Vec3> t(settings.volume.depth);
    std::vector<Vec3> s(settings.volume.depth);
    f32 worst = 0.0F;
    for (u32 y = 0; y < settings.volume.height; ++y) {
        for (u32 x = 0; x < settings.volume.width; ++x) {
            CY_REQUIRE(cy::rendering::fog::integrate_fog_column(
                           settings, view, fog.light, fog.shadow, HostShadowMap{}, medium, x, y,
                           Span<Vec3>(t), Span<Vec3>(s), &air)
                           .has_value());
            for (u32 slice = 0; slice < settings.volume.depth; ++slice) {
                const usize froxel =
                    (static_cast<usize>(slice) * settings.volume.height * settings.volume.width) +
                    (static_cast<usize>(y) * settings.volume.width) + x;
                const Vec3 device_t = table[5U + (froxel * 2U)].xyz();
                const Vec3 device_s = table[5U + (froxel * 2U) + 1U].xyz();
                worst = std::max({worst, relative_error(device_t, t[slice]),
                                  relative_error(device_s, s[slice])});
            }
        }
    }
    std::fprintf(stderr, "(f) the table variant against the host march over the same air: %.3g\n",
                 static_cast<double>(worst));
    CY_CHECK_LT(worst, 2e-3F);
    pass.destroy();
    device.destroy_buffer(*air_buffer);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
