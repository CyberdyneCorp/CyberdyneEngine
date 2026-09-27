// SPDX-License-Identifier: MIT
// Soft and contact shadows on a Vulkan device, with validation and synchronisation validation on.
// `render.soft_shadows`.
//
// ================================================================================================
// TWO HALVES, AND WHAT EACH ONE CAN FAIL ON
// ================================================================================================
//
// THE TRACE AGAINST ITS REFERENCE. contact_scene.h's analytic floor and box — depth and normals
// computed per pixel from the geometry — are uploaded and run through `ContactShadowPass` alone,
// and its target is compared with `contact_shadow_reference`, contact_shadows.slang transcribed.
//
// THE FRAME. `pipeline_test::FrameScene` — the one scene the pipeline suites render — given a
// directional shadow map through its recorder (the scene has none of its own) and three boxes: one
// resting on the floor, one of the same size hanging 1.6 m above it, and a small one resting near
// the camera. Every other box of the ring is moved out of the view and out of the shadow volume, so
// what a case measures is these three. Six properties:
//
//   (a) with the setting off the frame is BYTE-IDENTICAL to a committed reference drawn by the
//   frame
//       shader as it was before this change, and attaching the contact pass with its flag off
//       changes nothing either;
//   (b) the penumbra widens with the distance between blocker and receiver: the hanging box's
//       shadow has several times the penumbra of the resting box's, where the 3x3 filter gives the
//       two the same;
//   (c) no light leaks and no shadow speckles: floor the sun reaches past every box by more than
//       the widest kernel is exactly the unshadowed frame, and floor deep in an umbra is exactly
//       the frame with no sun, with the soft filter on;
//   (d) the contact term darkens where a coarse map misses the contact and nowhere else: every
//   pixel
//       that changes has the trace's term below one, every such term sits next to a box the
//       geometry puts within the trace's reach, and most contact pixels the map left lit go dark;
//   (e) the trace on the device is the host reference's.
//
// Lit, umbra and penumbra are read EXACTLY rather than by threshold: each configuration is rendered
// twice more — once with the shadow map switched off and once with the sun's intensity zeroed — and
// a pixel is lit when it equals the first, in umbra when it equals the second, and in penumbra when
// it is neither. The frame is deterministic, so equality is the right comparison.

#include "contact_scene.h"
#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/contact_shadows/contact_pass.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/lighting/soft_shadows.h>
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
using cy::rendering::contact_shadows::ContactShadowPass;
using cy::rendering::contact_shadows::ContactShadowPassDescription;
using cy::rendering::contact_shadows::ContactShadowSettings;

namespace {

/// The frame's set 0 texture-table slots the shadow map and the contact term are bound at. Any free
/// slots; the scene binds no material texture of its own.
constexpr u32 kShadowSlot = 121;
constexpr u32 kContactSlot = 122;
/// The shadow volume: an orthographic box `2 * kShadowRadius` metres across, centred on the floor.
constexpr f32 kShadowRadius = 9.0F;
constexpr f32 kShadowNear = 0.1F;
constexpr f32 kShadowFar = kShadowRadius * 4.0F;
const Vec3 kShadowCentre{0.0F, -1.8F, -7.0F};
/// The map every case but (d) draws with: 2048 texels over 18 m, 8.8 mm a texel.
constexpr u32 kFineExtent = 2048;
/// (d)'s: 14 cm a texel, too coarse for a contact — the scenario `virtual-shadows` names.
constexpr u32 kCoarseExtent = 128;
/// A large light — a 5.7-degree disc — so the penumbra is several pixels wide at this resolution.
/// The filter's width is proportional to it (`render_soft_shadows` measures that on the host).
constexpr f32 kAngularRadius = 0.05F;

constexpr f32 kFloorTop = -1.9F + 0.125F;
/// The three boxes. `kHang` is the gap between the hanging box and the floor.
constexpr f32 kHalf = 0.45F;
constexpr f32 kHang = 1.6F;
constexpr f32 kSmallHalf = 0.16F;
const Vec3 kRestingCentre{-1.6F, kFloorTop + kHalf, -5.8F};
const Vec3 kHangingCentre{1.3F, kFloorTop + kHalf + kHang, -6.6F};
const Vec3 kSmallCentre{0.1F, kFloorTop + kSmallHalf, -4.7F};

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
        description.application_name = "cy_test_render_soft_shadows";
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

void save(const char* name, Span<const u32> texels) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, texels, kWidth, kHeight).has_value()) {
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

/// `references/soft_shadows_off.png`: (a)'s frame — the shadow map on, the soft filter and the
/// contact term off — rendered by the frame shader as it was before this change: the suite run once
/// with `CY_RENDER_UPDATE_GOLDEN=1` against main's `frame_spirv.h` (0f1dfd1), before the shader
/// was regenerated. The change only appended `softShadowControl` and `softShadowShape` to the frame
/// block, so the old shader reads the same frame data.
const char* before_reference_path() noexcept {
    static char storage[1024];
    (void)std::snprintf(storage, sizeof(storage), "%s/references/soft_shadows_off.png",
                        CY_CONTACT_TEST_DIR);
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

/// What a frame case configures.
struct FrameCase {
    FrameScene* scene = nullptr;
    ContactShadowPass* pass = nullptr;
    rhi::TextureHandle shadow_color;
    rhi::TextureHandle shadow_depth;
    rhi::TextureViewHandle shadow_view;
    u32 extent = kFineExtent;
    /// The shadow map sampled at all. Off, the frame is the unshadowed one: the "lit" reference.
    bool shadowed = true;
    /// The sun's intensity zeroed: the "umbra" reference.
    bool dark = false;
    bool pcss = false;
    /// The contact pass attached — its stage declared and its target written.
    bool attached = false;
    /// `kSoftShadowContact` set, so the frame reads the term.
    bool contact = false;
    rendering::FrameResourceRead reads[2] = {};
    std::vector<rendering::GpuLight> lights;
    /// The sun's direction of travel as the frame uploaded it, so a case can check the geometry's
    /// rays travel the same way.
    Vec3 uploaded_sun{0.0F, 0.0F, 0.0F};
};

void configure_case(rendering::assembly::AssemblyDescription& description, void* user) noexcept {
    const auto* frame = static_cast<const FrameCase*>(user);
    description.pin_jitter = true;
    description.contact_shadows = frame->attached;
}

/// Three boxes where the cases want them, and the rest of the ring out of the view and out of the
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
        case 3:
            centre = kSmallCentre;
            half = kSmallHalf;
            return;
        default:
            centre = Vec3{static_cast<f32>(which) * 3.0F, 0.0F, 60.0F};
            half = 0.3F;
            return;
    }
}

/// The way the sun travels in these cases. FrameScene's own sun travels away from the camera, so
/// every shadow falls behind the box that casts it; the cases turn it round in the uploaded light
/// list — down, to the right and toward the camera — so the shadows fall on floor the camera sees.
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

Status before_assemble(rendering::RenderGraph& graph, rendering::assembly::AssemblyView& view,
                       rendering::assembly::FrameSinks& sinks, void* user) noexcept {
    auto* frame = static_cast<FrameCase*>(user);
    rendering::TextureRequest request;
    request.name = "soft shadow test map";
    request.format = rhi::Format::R32Sfloat;
    request.width = frame->extent;
    request.height = frame->extent;
    view.shadow_color =
        graph.import_texture(request, frame->shadow_color, rhi::ImageUse::Undefined);
    request.name = "soft shadow test depth";
    request.format = rhi::Format::D32Sfloat;
    view.shadow_depth =
        graph.import_texture(request, frame->shadow_depth, rhi::ImageUse::Undefined);
    // THE RECORDER HANDS OUT ITS SHADOW CALLBACK ONLY ONCE IT KNOWS THE TARGETS, and the scene took
    // its sinks before this hook ran; so the targets are set and the one callback is taken again.
    rendering::pipeline::FrameRecorder& recorder = frame->scene->recorder();
    recorder.set_shadow_targets(view.shadow_color, view.shadow_depth, frame->extent);
    const auto shadow = static_cast<usize>(rendering::FramePassKind::Shadow);
    sinks.passes[shadow] = recorder.sinks().passes[shadow];
    // The opaque pass samples the map (and the term) through the texture table, so it declares the
    // reads that make the graph transition them.
    const auto opaque = static_cast<usize>(rendering::FramePassKind::Opaque);
    frame->reads[0] =
        rendering::FrameResourceRead{view.shadow_color, rhi::Access::FragmentSampledRead};
    sinks.passes[opaque].reads = Span<const rendering::FrameResourceRead>(frame->reads, 1);

    if (!frame->attached) {
        return ok();
    }
    rendering::contact_shadows::ContactShadowView contact;
    contact.projection = frame->scene->projection();
    contact.relative_to_view = frame->scene->view();
    contact.to_light = sun_travel() * -1.0F;
    contact.width = kWidth;
    contact.height = kHeight;
    if (Status set = frame->pass->set_view(contact); !set) {
        return set;
    }
    view.contact_shadows = frame->pass->import_target(graph);
    sinks.contact_shadows = frame->pass->stage();
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
    auto* frame = static_cast<FrameCase*>(user);
    const u32 sun = directional_index(upload.lights);
    frame->lights.assign(upload.lights.begin(), upload.lights.end());
    if (sun < frame->lights.size()) {
        const Vec3 travel = sun_travel();
        frame->lights[sun].direction[0] = travel.x;
        frame->lights[sun].direction[1] = travel.y;
        frame->lights[sun].direction[2] = travel.z;
        if (frame->dark) {
            frame->lights[sun].intensity = 0.0F;
        }
        const f32* direction = frame->lights[sun].direction;
        frame->uploaded_sun = Vec3{direction[0], direction[1], direction[2]};
    }
    upload.lights = Span<const rendering::GpuLight>(frame->lights.data(), frame->lights.size());
    if (frame->attached) {
        // The jitter is known once the frame is assembled, and the trace reads its constants when
        // it records — `render.ambient_occlusion` sets its view again here for the same reason.
        rendering::contact_shadows::ContactShadowView contact;
        contact.projection = frame->scene->projection();
        contact.relative_to_view = frame->scene->view();
        contact.to_light = sun_travel() * -1.0F;
        contact.width = kWidth;
        contact.height = kHeight;
        contact.jitter = Vec2{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]};
        if (Status set = frame->pass->set_view(contact); !set) {
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
    upload.view.shadow_control[2] = frame->extent;
    upload.view.shadow_control[3] = frame->shadowed ? 1U : 0U;

    u32 flags = 0;
    flags |= frame->pcss ? rendering::pipeline::kSoftShadowPcss : 0U;
    flags |= frame->contact ? rendering::pipeline::kSoftShadowContact : 0U;
    if (flags != 0U) {
        rendering::SoftShadowSettings settings;
        settings.angular_radius = kAngularRadius;
        rendering::DirectionalShadowFootprint footprint;
        footprint.depth_range_metres = kShadowFar - kShadowNear;
        footprint.width_metres = kShadowRadius * 2.0F;
        footprint.extent = frame->extent;
        rendering::write_soft_shadow_words(
            flags, frame->attached ? kContactSlot : rendering::pipeline::kNoMaterialTexture,
            rendering::make_pcss_shape(settings, footprint), upload.view.soft_shadow_control,
            upload.view.soft_shadow_shape);
    }

    rendering::pipeline::MaterialTextureSlot slots[2] = {
        {kShadowSlot, frame->shadow_view},
        {kContactSlot, frame->attached ? frame->pass->target_view() : rhi::TextureViewHandle{}},
    };
    return frame->scene->set_frame_textures(
        Span<const rendering::pipeline::MaterialTextureSlot>(slots, frame->attached ? 2U : 1U));
}

/// One scene, built with a case's hooks, and the frame it rendered.
class FrameRun {
public:
    struct Options {
        u32 extent = kFineExtent;
        bool shadowed = true;
        bool dark = false;
        bool pcss = false;
        bool attached = false;
        bool contact = false;
    };

    FrameRun(DeviceFixture& fixture, const Options& options)
        : device_(&fixture.device()), scene_(allocator()) {
        frame_.scene = &scene_;
        frame_.pass = &pass_;
        frame_.extent = options.extent;
        frame_.shadowed = options.shadowed;
        frame_.dark = options.dark;
        frame_.pcss = options.pcss;
        frame_.attached = options.attached;
        frame_.contact = options.contact;
        ready_ = create_shadow_map();
        if (options.attached) {
            ContactShadowPassDescription description;
            description.width = kWidth;
            description.height = kHeight;
            description.readback = true;
            ready_ = ready_ && pass_.create(allocator(), fixture.device(), description).has_value();
            pass_.set_settings(ContactShadowSettings{});
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
            std::fprintf(stderr, "soft shadow frame: build failed: %s\n", built.error().message);
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
            std::fprintf(stderr, "soft shadow frame: render failed: %s\n",
                         rendered.error().message);
            return false;
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        if (frame_.attached) {
            term_.assign(static_cast<usize>(kWidth) * kHeight, 0.0F);
            return pass_.read_back(Span<f32>(term_.data(), term_.size())).has_value();
        }
        return true;
    }

    [[nodiscard]] const std::vector<u32>& pixels() const { return pixels_; }
    [[nodiscard]] const std::vector<f32>& term() const { return term_; }
    [[nodiscard]] const rendering::assembly::AssemblyReport& report() const { return report_; }
    [[nodiscard]] FrameScene& scene() { return scene_; }
    [[nodiscard]] Vec3 uploaded_sun() const { return frame_.uploaded_sun; }

private:
    [[nodiscard]] bool create_shadow_map() {
        rhi::TextureDescription texture;
        texture.name = "soft shadow test map";
        texture.format = rhi::Format::R32Sfloat;
        texture.extent = rhi::Extent3D{frame_.extent, frame_.extent, 1};
        texture.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        auto color = device_->create_texture(texture);
        if (!color.has_value()) {
            return false;
        }
        frame_.shadow_color = *color;
        texture.name = "soft shadow test depth";
        texture.format = rhi::Format::D32Sfloat;
        texture.usage = rhi::TextureUsage::DepthStencilAttachment;
        auto depth = device_->create_texture(texture);
        if (!depth.has_value()) {
            return false;
        }
        frame_.shadow_depth = *depth;
        rhi::TextureViewDescription view;
        view.name = "soft shadow test map";
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
    ContactShadowPass pass_;
    FrameCase frame_;
    rendering::assembly::AssemblyReport report_{};
    std::vector<u32> pixels_;
    std::vector<f32> term_;
    bool ready_ = false;
};

/// A configuration and its two references, rendered: the frame, the frame with the shadow map off
/// (lit everywhere) and the frame with the sun off (umbra everywhere).
struct Classified {
    std::vector<u32> frame;
    std::vector<u32> lit;
    std::vector<u32> dark;

    [[nodiscard]] bool penumbra(usize pixel) const noexcept {
        return frame[pixel] != lit[pixel] && frame[pixel] != dark[pixel];
    }
};

[[nodiscard]] bool classify(DeviceFixture& fixture, FrameRun::Options options, Classified& out) {
    FrameRun frame(fixture, options);
    options.shadowed = false;
    FrameRun lit(fixture, options);
    options.shadowed = true;
    options.dark = true;
    FrameRun dark(fixture, options);
    if (!frame.render() || !lit.render() || !dark.render()) {
        return false;
    }
    out.frame = frame.pixels();
    out.lit = lit.pixels();
    out.dark = dark.pixels();
    return true;
}

// --- The geometry's answers ----------------------------------------------------------------------

[[nodiscard]] Vec3 pixel_ray(const Mat4& projection, u32 x, u32 y) noexcept {
    const f32 ndc_x = (((static_cast<f32>(x) + 0.5F) / kWidth) * 2.0F) - 1.0F;
    const f32 ndc_y = 1.0F - (((static_cast<f32>(y) + 0.5F) / kHeight) * 2.0F);
    return Vec3{ndc_x / projection.columns[0].x, ndc_y / projection.columns[1].y, -1.0F};
}

[[nodiscard]] f32 ray_aabb(Vec3 origin, Vec3 direction, const Aabb& box) noexcept {
    const contact_test::Box shape{box.min, box.max};
    Vec3 normal{0.0F, 0.0F, 0.0F};
    return contact_test::ray_box(origin, direction, shape, normal);
}

/// The floor-top point a pixel sees, or false when the pixel sees a box or nothing.
[[nodiscard]] bool floor_point(FrameScene& scene, u32 x, u32 y, Vec3& point) noexcept {
    const Span<const Aabb> boxes = scene.boxes();
    const Vec3 ray = pixel_ray(scene.projection(), x, y);
    const Vec3 origin{0.0F, 0.0F, 0.0F};
    const f32 floor_t = ray_aabb(origin, ray, boxes[0]);
    for (usize index = 1; index < boxes.size(); ++index) {
        if (ray_aabb(origin, ray, boxes[index]) < floor_t) {
            return false;
        }
    }
    if (!std::isfinite(floor_t)) {
        return false;
    }
    point = ray * floor_t;
    return std::fabs(point.y - kFloorTop) < 1.0e-3F;
}

/// The box the sun's ray from `point` meets first, within `reach` metres, or -1. The floor slab is
/// not a blocker of its own top.
[[nodiscard]] i32 sun_blocker(FrameScene& scene, Vec3 point, f32 reach) noexcept {
    const Span<const Aabb> boxes = scene.boxes();
    const Vec3 to_sun = sun_travel() * -1.0F;
    const Vec3 start = point + Vec3{0.0F, 1.0e-3F, 0.0F};
    i32 hit = -1;
    f32 nearest = reach;
    for (usize index = 1; index < boxes.size(); ++index) {
        const f32 t = ray_aabb(start, to_sun, boxes[index]);
        if (std::isfinite(t) && t <= nearest) {
            nearest = t;
            hit = static_cast<i32>(index);
        }
    }
    return hit;
}

/// Whether the sun's rays from `point` and from eight points `margin` metres around it all miss
/// (`want_blocked` false) or all meet a box (`want_blocked` true).
[[nodiscard]] bool uniformly(FrameScene& scene, Vec3 point, f32 margin,
                             bool want_blocked) noexcept {
    for (i32 dz = -1; dz <= 1; ++dz) {
        for (i32 dx = -1; dx <= 1; ++dx) {
            const Vec3 moved =
                point + Vec3{static_cast<f32>(dx) * margin, 0.0F, static_cast<f32>(dz) * margin};
            if ((sun_blocker(scene, moved, INFINITY) >= 0) != want_blocked) {
                return false;
            }
        }
    }
    return true;
}

/// The distance from a point to the nearest box that is not the floor slab, in metres.
[[nodiscard]] f32 clearance(FrameScene& scene, Vec3 point) noexcept {
    const Span<const Aabb> boxes = scene.boxes();
    f32 nearest = INFINITY;
    for (usize index = 1; index < boxes.size(); ++index) {
        const Aabb& box = boxes[index];
        const f32 dx = std::max({box.min.x - point.x, 0.0F, point.x - box.max.x});
        const f32 dy = std::max({box.min.y - point.y, 0.0F, point.y - box.max.y});
        const f32 dz = std::max({box.min.z - point.z, 0.0F, point.z - box.max.z});
        nearest = std::min(nearest, std::sqrt((dx * dx) + (dy * dy) + (dz * dz)));
    }
    return nearest;
}

[[nodiscard]] usize differing(const std::vector<u32>& a, const std::vector<u32>& b) noexcept {
    usize count = 0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        count += static_cast<usize>(a[index] != b[index]);
    }
    return count + (a.size() > b.size() ? a.size() - b.size() : b.size() - a.size());
}

[[nodiscard]] i32 brightness(u32 texel) noexcept {
    return static_cast<i32>(texel & 0xFFU) + static_cast<i32>((texel >> 8U) & 0xFFU) +
           static_cast<i32>((texel >> 16U) & 0xFFU);
}

}  // namespace

CY_TEST_CASE("(a) soft shadows off is the frame before the change, byte for byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameRun off(fixture, FrameRun::Options{});
    CY_REQUIRE(off.render());
    save("soft-shadows-closeup-before.png",
         Span<const u32>(off.pixels().data(), off.pixels().size()));
    check_against_before(off.pixels());

    // The contact pass attached, its stage declared and its target written, and the flag off: the
    // frame does not read the term, so nothing changes.
    FrameRun::Options attached;
    attached.attached = true;
    FrameRun unread(fixture, attached);
    CY_REQUIRE(unread.render());
    const usize changed = differing(off.pixels(), unread.pixels());
    std::fprintf(stderr, "(a) %zu pixels differ with the contact pass attached and unread\n",
                 changed);
    CY_CHECK_EQ(changed, usize{0});
    CY_CHECK_GT(unread.report().passes_declared, off.report().passes_declared);

    // The control: both switched on draw a different frame — the close-up this change publishes.
    FrameRun::Options soft;
    soft.pcss = true;
    soft.attached = true;
    soft.contact = true;
    FrameRun on(fixture, soft);
    CY_REQUIRE(on.render());
    save("soft-shadows-closeup-after.png", Span<const u32>(on.pixels().data(), on.pixels().size()));
    CY_CHECK_GT(differing(off.pixels(), on.pixels()), usize{100});
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) the penumbra widens with the distance between blocker and receiver") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameRun::Options soft;
    soft.pcss = true;
    Classified pcss;
    Classified hard;
    CY_REQUIRE(classify(fixture, soft, pcss));
    CY_REQUIRE(classify(fixture, FrameRun::Options{}, hard));
    save("soft-shadows-pcss.png", Span<const u32>(pcss.frame.data(), pcss.frame.size()));

    // Each floor pixel in penumbra is credited to the box whose shadow it borders: the box the
    // sun's ray from it, or from a point half a metre around it, meets. The geometry's rays must
    // travel the way the frame's sun does, or the credit is to the wrong box.
    FrameRun probe(fixture, FrameRun::Options{});
    CY_REQUIRE(probe.render());
    const Vec3 uploaded = probe.uploaded_sun();
    const Vec3 assumed = sun_travel();
    const bool same_sun = std::fabs(uploaded.x - assumed.x) < 1.0e-4F &&
                          std::fabs(uploaded.y - assumed.y) < 1.0e-4F &&
                          std::fabs(uploaded.z - assumed.z) < 1.0e-4F;
    CY_REQUIRE(same_sun);
    u32 soft_counts[3] = {};
    u32 hard_counts[3] = {};
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            Vec3 point{0.0F, 0.0F, 0.0F};
            if (!floor_point(probe.scene(), x, y, point)) {
                continue;
            }
            // The pixel's own ray first, then the eight around it.
            i32 owner = sun_blocker(probe.scene(), point, INFINITY);
            for (i32 dz = -1; dz <= 1 && owner < 0; ++dz) {
                for (i32 dx = -1; dx <= 1 && owner < 0; ++dx) {
                    owner = sun_blocker(probe.scene(),
                                        point + Vec3{static_cast<f32>(dx) * 0.5F, 0.0F,
                                                     static_cast<f32>(dz) * 0.5F},
                                        INFINITY);
                }
            }
            if (owner != 1 && owner != 2) {
                continue;
            }
            const usize pixel = (static_cast<usize>(y) * kWidth) + x;
            soft_counts[owner] += pcss.penumbra(pixel) ? 1U : 0U;
            hard_counts[owner] += hard.penumbra(pixel) ? 1U : 0U;
        }
    }
    std::fprintf(stderr,
                 "(b) penumbra pixels: resting box %u (3x3 filter %u), hanging box %u (3x3 "
                 "filter %u)\n",
                 soft_counts[1], hard_counts[1], soft_counts[2], hard_counts[2]);
    CY_REQUIRE(soft_counts[1] > 0U);
    CY_REQUIRE(hard_counts[1] > 0U);
    // MEASURED on the reference machine: 346 and 1710 penumbra pixels with the soft filter, 121 and
    // 153 with the 3x3. The hanging box's shadow is 1.6 m from the floor and the resting box's
    // meets it, so similar triangles give the first a band several times the second's; the 3x3
    // filter gives both about a texel.
    CY_CHECK_GT(soft_counts[2], 3U * soft_counts[1]);
    CY_CHECK_GT(soft_counts[2], 3U * hard_counts[2]);
    CY_CHECK_LT(hard_counts[2], 2U * hard_counts[1]);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) no light leaks into an umbra and no shadow speckles an open floor") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameRun::Options soft;
    soft.pcss = true;
    Classified pcss;
    CY_REQUIRE(classify(fixture, soft, pcss));
    FrameRun probe(fixture, FrameRun::Options{});

    // The widest kernel — and the widest blocker search — the filter can reach, in metres: 24
    // texels of 8.8 mm, with the lookup's normal offset and a pixel of slack on top.
    constexpr f32 kMargin = 0.25F;
    u32 open = 0;
    u32 open_changed = 0;
    u32 umbra = 0;
    u32 umbra_changed = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            Vec3 point{0.0F, 0.0F, 0.0F};
            if (!floor_point(probe.scene(), x, y, point)) {
                continue;
            }
            const usize pixel = (static_cast<usize>(y) * kWidth) + x;
            // Two pixels in from the slab's edges, where the map's texels straddle its side.
            const Aabb& slab = probe.scene().boxes()[0];
            if (point.x < slab.min.x + 0.2F || point.x > slab.max.x - 0.2F ||
                point.z < slab.min.z + 0.2F || point.z > slab.max.z - 0.2F) {
                continue;
            }
            // And a few centimetres from every box's foot: a pixel on a box's silhouette is the
            // box's in the frame and the floor's to a ray through the pixel's centre, depending on
            // where the jittered rasteriser sampled it.
            if (clearance(probe.scene(), point) < 0.05F) {
                continue;
            }
            if (uniformly(probe.scene(), point, kMargin, false)) {
                ++open;
                open_changed += pcss.frame[pixel] != pcss.lit[pixel] ? 1U : 0U;
            } else if (uniformly(probe.scene(), point, kMargin, true)) {
                ++umbra;
                umbra_changed += pcss.frame[pixel] != pcss.dark[pixel] ? 1U : 0U;
            }
        }
    }
    std::fprintf(stderr,
                 "(c) %u open floor pixels, %u not exactly lit; %u umbra pixels, %u not exactly "
                 "dark\n",
                 open, open_changed, umbra, umbra_changed);
    CY_REQUIRE(open > 5000U);
    CY_REQUIRE(umbra > 200U);
    CY_CHECK_EQ(open_changed, 0U);
    CY_CHECK_EQ(umbra_changed, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "(d) the contact term darkens where a coarse map misses the contact, and nowhere else") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameRun::Options coarse;
    coarse.extent = kCoarseExtent;
    FrameRun::Options with_contact = coarse;
    with_contact.attached = true;
    with_contact.contact = true;
    FrameRun off(fixture, coarse);
    FrameRun on(fixture, with_contact);
    FrameRun::Options dark_options = coarse;
    dark_options.dark = true;
    FrameRun dark(fixture, dark_options);
    CY_REQUIRE(off.render());
    CY_REQUIRE(on.render());
    CY_REQUIRE(dark.render());
    save("contact-shadows-off.png", Span<const u32>(off.pixels().data(), off.pixels().size()));
    save("contact-shadows-on.png", Span<const u32>(on.pixels().data(), on.pixels().size()));

    const ContactShadowSettings settings;
    u32 changed = 0;
    u32 changed_without_term = 0;
    u32 brighter = 0;
    u32 traced = 0;
    u32 traced_far_from_box = 0;
    u32 missed = 0;
    u32 missed_found = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize pixel = (static_cast<usize>(y) * kWidth) + x;
            if (off.pixels()[pixel] != on.pixels()[pixel]) {
                ++changed;
                changed_without_term += on.term()[pixel] < 1.0F ? 0U : 1U;
                brighter +=
                    brightness(on.pixels()[pixel]) > brightness(off.pixels()[pixel]) ? 1U : 0U;
            }
            if (on.term()[pixel] < 1.0F) {
                ++traced;
                // NOWHERE ELSE: the geometry puts a box within the trace's reach of this pixel or
                // of one of its neighbours — a pixel of slack for the jitter the depth was drawn
                // with, which the geometry's rays do not have.
                bool near_box = false;
                for (i32 dy = -1; dy <= 1 && !near_box; ++dy) {
                    for (i32 dx = -1; dx <= 1 && !near_box; ++dx) {
                        const i32 nx = static_cast<i32>(x) + dx;
                        const i32 ny = static_cast<i32>(y) + dy;
                        if (nx < 0 || ny < 0 || std::cmp_greater_equal(nx, kWidth) ||
                            std::cmp_greater_equal(ny, kHeight)) {
                            continue;
                        }
                        Vec3 point{0.0F, 0.0F, 0.0F};
                        if (!floor_point(on.scene(), static_cast<u32>(nx), static_cast<u32>(ny),
                                         point)) {
                            near_box = true;  // a box's own face: the trace is on the box
                            continue;
                        }
                        // Within the trace's reach of a box — toward the light, or beside the box's
                        // silhouette, where a tap behind the box's front face by less than the
                        // thickness is read as inside it — and nowhere farther.
                        near_box = sun_blocker(on.scene(), point, settings.length * 1.5F) >= 0 ||
                                   clearance(on.scene(), point) <= settings.length;
                    }
                }
                traced_far_from_box += near_box ? 0U : 1U;
            }
            // WHERE THE MAP MISSES IT: floor the geometry puts in contact shadow — a box within the
            // first half of the trace — that the coarse map left brighter than the sunless frame.
            Vec3 point{0.0F, 0.0F, 0.0F};
            if (floor_point(on.scene(), x, y, point) &&
                sun_blocker(on.scene(), point, settings.length * 0.45F) >= 0 &&
                off.pixels()[pixel] != dark.pixels()[pixel]) {
                ++missed;
                missed_found +=
                    brightness(on.pixels()[pixel]) < brightness(off.pixels()[pixel]) ? 1U : 0U;
            }
        }
    }
    std::fprintf(stderr,
                 "(d) %u pixels changed (%u without the term below one, %u brighter); %u traced "
                 "(%u far from any box); %u contact pixels the map missed, %u darkened\n",
                 changed, changed_without_term, brighter, traced, traced_far_from_box, missed,
                 missed_found);
    CY_REQUIRE(missed > 50U);
    CY_CHECK_EQ(changed_without_term, 0U);
    CY_CHECK_EQ(brighter, 0U);
    CY_CHECK_EQ(traced_far_from_box, 0U);
    // MEASURED on the reference machine: 124 of the 132 go dark. The rest are floor the trace
    // reaches the box behind a face turned away from the camera, which no screen-space trace can
    // see.
    CY_CHECK_GE(static_cast<f32>(missed_found), 0.8F * static_cast<f32>(missed));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(e) the trace on the device is the host reference's") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    rhi::Device& device = fixture.device();
    const contact_test::ContactScene scene = contact_test::make_contact_scene(true);
    const u32 width = scene.view.width;
    const u32 height = scene.view.height;

    ContactShadowPass pass;
    ContactShadowPassDescription description;
    description.width = width;
    description.height = height;
    description.readback = true;
    CY_REQUIRE(pass.create(allocator(), device, description).has_value());
    CY_REQUIRE(pass.set_view(scene.view).has_value());

    struct Upload {
        rhi::TextureHandle texture;
        rhi::BufferHandle staging;
        rendering::ResourceId resource = rendering::kInvalidResource;
    };
    std::vector<Vec4> normals(scene.normals.size());
    for (usize pixel = 0; pixel < normals.size(); ++pixel) {
        normals[pixel] = Vec4{scene.normals[pixel].x, scene.normals[pixel].y, 1.0F, 1.0F};
    }
    Upload uploads[2];
    const rhi::Format formats[2] = {rhi::Format::R32Sfloat, rhi::Format::Rgba32Sfloat};
    const void* sources[2] = {scene.depth.data(), normals.data()};
    const u64 sizes[2] = {scene.depth.size() * sizeof(f32), normals.size() * sizeof(Vec4)};
    for (u32 index = 0; index < 2; ++index) {
        rhi::TextureDescription texture;
        texture.name = "contact test input";
        texture.format = formats[index];
        texture.extent = rhi::Extent3D{width, height, 1};
        texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
        auto created = device.create_texture(texture);
        CY_REQUIRE(created.has_value());
        uploads[index].texture = *created;
        rhi::BufferDescription staging;
        staging.name = "contact test staging";
        staging.size = sizes[index];
        staging.usage = rhi::BufferUsage::TransferSource;
        staging.memory = rhi::MemoryUse::Upload;
        auto buffer = device.create_buffer(staging);
        CY_REQUIRE(buffer.has_value());
        uploads[index].staging = *buffer;
        void* mapped = device.buffer_mapped_pointer(*buffer);
        CY_REQUIRE(mapped != nullptr);
        std::memcpy(mapped, sources[index], sizes[index]);
    }

    struct Copy {
        Upload* uploads = nullptr;
        u32 width = 0;
        u32 height = 0;
    } copy{uploads, width, height};
    const auto record_upload = [](const rendering::PassContext& context, void* user) noexcept {
        const auto* job = static_cast<const Copy*>(user);
        for (u32 index = 0; index < 2; ++index) {
            rhi::BufferTextureCopy region;
            region.texture_extent = rhi::Extent3D{job->width, job->height, 1};
            context.commands->copy_buffer_to_texture(
                job->uploads[index].staging,
                context.executor->texture(job->uploads[index].resource),
                Span<const rhi::BufferTextureCopy>(&region, 1));
        }
    };

    bool ran = false;
    if (device.begin_frame().has_value()) {
        rendering::RenderGraph graph(allocator());
        rendering::TextureRequest request;
        request.width = width;
        request.height = height;
        request.name = "contact test depth";
        request.format = formats[0];
        uploads[0].resource =
            graph.import_texture(request, uploads[0].texture, rhi::ImageUse::Undefined);
        request.name = "contact test normals";
        request.format = formats[1];
        uploads[1].resource =
            graph.import_texture(request, uploads[1].texture, rhi::ImageUse::Undefined);
        graph.add_pass("contact test upload", rhi::QueueKind::Graphics)
            .write(uploads[0].resource, rhi::Access::TransferWrite)
            .write(uploads[1].resource, rhi::Access::TransferWrite)
            .record(+record_upload, &copy);
        rendering::ScreenSpaceStageInputs stage;
        stage.depth = uploads[0].resource;
        stage.normal_roughness = uploads[1].resource;
        stage.target = pass.import_target(graph);
        stage.width = width;
        stage.height = height;
        const bool declared = pass.declare(graph, stage) != rendering::kInvalidPass;
        rendering::GraphExecutor executor(allocator(), device);
        ran = declared &&
              executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{})
                  .has_value() &&
              device.wait_idle().has_value();
        executor.release();
        ran = device.end_frame().has_value() && ran;
    }
    CY_REQUIRE(ran);
    std::vector<f32> term(scene.depth.size(), 0.0F);
    CY_REQUIRE(pass.read_back(Span<f32>(term.data(), term.size())).has_value());

    rendering::contact_shadows::ContactShadowInputs inputs;
    inputs.width = width;
    inputs.height = height;
    inputs.depth = Span<const f32>(scene.depth.data(), scene.depth.size());
    inputs.normals = Span<const Vec2>(scene.normals.data(), scene.normals.size());
    std::vector<f32> reference(scene.depth.size(), 0.0F);
    CY_REQUIRE(rendering::contact_shadows::contact_shadow_reference(
                   inputs, pass.constants(), Span<f32>(reference.data(), reference.size()))
                   .has_value());
    // The target is 8-bit: half a step of rounding is the whole of the expected difference. A pixel
    // whose tap sits within float rounding of a depth sample may resolve the other way on the
    // device, so a handful beyond that is counted and bounded rather than forbidden. MEASURED:
    // none.
    u32 beyond = 0;
    u32 shadowed = 0;
    for (usize pixel = 0; pixel < term.size(); ++pixel) {
        beyond += std::fabs(term[pixel] - reference[pixel]) > (0.5F / 255.0F) + 1.0e-4F ? 1U : 0U;
        shadowed += reference[pixel] < 1.0F ? 1U : 0U;
    }
    std::fprintf(stderr,
                 "(e) %u of %zu pixels differ from the host by more than half a step; %u "
                 "in contact shadow on the host\n",
                 beyond, term.size(), shadowed);
    CY_CHECK_GT(shadowed, 100U);
    CY_CHECK_LE(beyond, 4U);
    for (Upload& upload : uploads) {
        device.destroy_texture(upload.texture);
        device.destroy_buffer(upload.staging);
    }
    pass.destroy();
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
