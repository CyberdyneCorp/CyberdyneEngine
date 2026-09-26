// SPDX-License-Identifier: MIT
// Ambient occlusion on a Vulkan device, with validation and synchronisation validation on.
// `render.ambient_occlusion`.
//
// ================================================================================================
// TWO HALVES, AND WHAT EACH ONE CAN FAIL ON
// ================================================================================================
//
// THE PASS AGAINST ITS REFERENCES. An analytic inner corner — a floor meeting a wall, depth and
// normals computed per pixel from the planes — is uploaded and run through the five dispatches
// alone. The horizon search's output is compared against `gtao_reference`, gtao.slang transcribed
// on the host; the filtered term against `denoise::Denoiser::denoise` run over THAT SAME raw
// output with the ambient occlusion signal's own configuration. The second comparison is what
// makes "filtered by the shared denoiser" a measurement.
//
// THE FRAME. `pipeline_test::FrameScene` — the one scene the pipeline suites render: eleven boxes
// on a floor slab, a sun and two point lights — with the stage switched on through the post chain's
// own setting, the target imported, and the forward pass reading it from the frame's texture table.
// Five cases, one per property the requirement states:
//
//   (a) a known contact — the floor in front of a box that rests on it — is darker with AO on;
//   (b) open floor, farther than the radius from every box, is unchanged within one 8-bit step;
//   (c) with the ambient term zeroed, the frame is BYTE-IDENTICAL with AO on and off: every pixel
//       is then direct light alone, and AO must not touch it;
//   (d) AO off with the pass attached is byte-identical to the scene with no pass at all, with the
//       same number of declared passes, and that scene matches a committed reference rendered by
//       the frame shader as it was before the stage (main's SPIR-V), so a change to the path both
//       share cannot pass unseen;
//   (e) four frames of a still view: the term moves by at most `kStableTolerance` between frames
//       and the resolved frame by at most one 8-bit step. Rendered through the scene's temporal
//       anti-aliasing with the jitter's spread set to zero, because the jitter sequence moves the
//       depth a sub-pixel each frame even when pinned — the term then follows the jittered depth,
//       and that motion is the resolve's to average, not noise the search adds. A second case
//       keeps the jitter and bounds the resolved frame's frame-to-frame change over the occluded
//       pixels the frame without the stage leaves still.
//
// TOLERANCES ARE STATED WHERE THEY ARE USED, with the measured value beside each.

#include "corner_scene.h"
#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/occlusion/occlusion_pass.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace cy;
using namespace cy::occlusion_test;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using cy::rendering::occlusion::AmbientOcclusionPass;
using cy::rendering::occlusion::AmbientOcclusionPassDescription;

namespace {

/// Half precision, which the target and the intermediates are stored in, is 2^-11 relative at 1.
/// MEASURED on the reference machine: 4.9e-4 worst difference for the search, the half-rounding of
/// the output alone. Four times that, because the device's `acos` and `pow` are not the host's.
constexpr f32 kSearchTolerance = 2.0e-3F;
/// The cascade reads its previous pass back at half precision three times where the host carries
/// a float. MEASURED on the visibility: 2.7e-4 mean, 1.5e-3 worst.
constexpr f32 kFilterTolerance = 4.0e-3F;
/// The bent normal's components are filtered by the same weights but span [-1, 1], where one
/// half-precision step is eight times the step at the visibility's usual values. MEASURED: 1.4e-2.
constexpr f32 kBentTolerance = 3.0e-2F;
/// (e): the term's largest change between two frames of a still view. MEASURED: exactly zero — the
/// search's noise is fixed in screen space and the pass reads nothing that changes.
constexpr f32 kStableTolerance = 1.0e-3F;
/// (e) through the temporal resolve, over occluded pixels the frame without the stage leaves
/// still, with the ambient term scaled up so it is what those pixels show. MEASURED: the resolved
/// colour moves by 0.023 of an 8-bit step per channel on average and the term by 0.039 — the jitter
/// moving the depth under a term that follows it. With the cascade removed (the raw search written
/// to the target) the same frames measure 0.043 and 0.051; the bounds sit between the two.
constexpr f32 kCrawlOccluded = 0.95F;
constexpr f32 kCrawlAmbient = 8.0F;
constexpr f64 kCrawlPixel = 0.035;
constexpr f64 kCrawlTerm = 0.045;
/// The frame's slot of set 0's texture table the term is bound at. Any free slot; the scene binds
/// no material texture of its own in these cases.
constexpr u32 kOcclusionSlot = 120;
/// The radius the frame cases search, in metres. The scene's boxes are 0.7 to 1.4 m across.
constexpr f32 kFrameRadius = 0.5F;

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
        description.application_name = "cy_test_render_ambient_occlusion";
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

/// `references/ambient_occlusion_absent.png`: the (d) scene with the stage absent, rendered by
/// the frame shader as it was before this stage — the SPIR-V of main at 2614fb0 substituted for
/// `frame_spirv.h` and the suite run once with `CY_RENDER_UPDATE_GOLDEN=1`. The shader only
/// appended `occlusionControl` to the frame block, so the old shader reads the same frame data.
const char* before_reference_path() noexcept {
    static char storage[1024];
    (void)std::snprintf(storage, sizeof(storage), "%s/references/ambient_occlusion_absent.png",
                        CY_OCCLUSION_TEST_DIR);
    return storage;
}

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// The committed-reference comparison `tests/render/golden.h` defines, with its derived tolerance
/// and the stronger zero-difference claim on the machine the reference came from.
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
                 "(d) against the frame before the stage: %u differing (%u off edge), edge budget "
                 "%u, worst delta %u at (%u, %u)\n",
                 comparison.differing, comparison.differing_off_edge, comparison.edge_texels,
                 comparison.max_channel_delta, comparison.worst_x, comparison.worst_y);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing_off_edge, 0U);
    CY_CHECK_LE(comparison.differing, comparison.edge_texels);
    CY_CHECK_EQ(comparison.differing, 0U);
}

// --- The pass against its references ---------------------------------------------------------

/// A device texture holding host data, and the upload that fills it.
struct UploadedTexture {
    rhi::TextureHandle texture;
    rhi::BufferHandle staging;
    rendering::ResourceId resource = rendering::kInvalidResource;
    u32 width = 0;
    u32 height = 0;
};

[[nodiscard]] bool make_uploaded(rhi::Device& device, rhi::Format format, const void* data,
                                 u64 bytes, UploadedTexture& out) {
    rhi::TextureDescription texture;
    texture.name = "occlusion test input";
    texture.format = format;
    texture.extent = rhi::Extent3D{kSceneWidth, kSceneHeight, 1};
    texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
    auto created = device.create_texture(texture);
    if (!created.has_value()) {
        return false;
    }
    out.texture = *created;
    rhi::BufferDescription staging;
    staging.name = "occlusion test staging";
    staging.size = bytes;
    staging.usage = rhi::BufferUsage::TransferSource;
    staging.memory = rhi::MemoryUse::Upload;
    auto buffer = device.create_buffer(staging);
    if (!buffer.has_value()) {
        return false;
    }
    out.staging = *buffer;
    void* mapped = device.buffer_mapped_pointer(out.staging);
    if (mapped == nullptr) {
        return false;
    }
    std::memcpy(mapped, data, bytes);
    out.width = kSceneWidth;
    out.height = kSceneHeight;
    return true;
}

void record_upload(const rendering::PassContext& context, void* user) noexcept {
    auto* textures = static_cast<UploadedTexture*>(user);
    for (u32 index = 0; index < 2; ++index) {
        rhi::BufferTextureCopy region;
        region.texture_extent = rhi::Extent3D{textures[index].width, textures[index].height, 1};
        context.commands->copy_buffer_to_texture(
            textures[index].staging, context.executor->texture(textures[index].resource),
            Span<const rhi::BufferTextureCopy>(&region, 1));
    }
}

struct PassRun {
    std::vector<Vec4> raw;
    std::vector<Vec4> filtered;
};

[[nodiscard]] bool run_pass(DeviceFixture& fixture, const CornerScene& scene, PassRun& out) {
    rhi::Device& device = fixture.device();
    AmbientOcclusionPass pass;
    AmbientOcclusionPassDescription description;
    description.width = kSceneWidth;
    description.height = kSceneHeight;
    description.readback = true;
    if (!pass.create(allocator(), device, description).has_value()) {
        return false;
    }
    rendering::occlusion::GtaoSettings settings;
    settings.shared.radius = 1.0F;
    pass.set_settings(settings);
    if (!pass.set_view(scene.view).has_value()) {
        return false;
    }

    std::vector<Vec4> normals(scene.normals.size());
    for (usize pixel = 0; pixel < normals.size(); ++pixel) {
        normals[pixel] = Vec4{scene.normals[pixel].x, scene.normals[pixel].y, 1.0F, 1.0F};
    }
    UploadedTexture inputs[2];
    if (!make_uploaded(device, rhi::Format::R32Sfloat, scene.depth.data(),
                       scene.depth.size() * sizeof(f32), inputs[0]) ||
        !make_uploaded(device, rhi::Format::Rgba32Sfloat, normals.data(),
                       normals.size() * sizeof(Vec4), inputs[1])) {
        return false;
    }

    bool ran = false;
    if (device.begin_frame().has_value()) {
        rendering::RenderGraph graph(allocator());
        rendering::TextureRequest request;
        request.width = kSceneWidth;
        request.height = kSceneHeight;
        request.name = "occlusion test depth";
        request.format = rhi::Format::R32Sfloat;
        inputs[0].resource =
            graph.import_texture(request, inputs[0].texture, rhi::ImageUse::Undefined);
        request.name = "occlusion test normals";
        request.format = rhi::Format::Rgba32Sfloat;
        inputs[1].resource =
            graph.import_texture(request, inputs[1].texture, rhi::ImageUse::Undefined);
        graph.add_pass("occlusion test upload", rhi::QueueKind::Graphics)
            .write(inputs[0].resource, rhi::Access::TransferWrite)
            .write(inputs[1].resource, rhi::Access::TransferWrite)
            .record(&record_upload, inputs);

        rendering::ScreenSpaceStageInputs stage;
        stage.depth = inputs[0].resource;
        stage.normal_roughness = inputs[1].resource;
        stage.target = pass.import_target(graph);
        stage.width = kSceneWidth;
        stage.height = kSceneHeight;
        const bool declared = pass.declare(graph, stage) != rendering::kInvalidPass;
        rendering::GraphExecutor executor(allocator(), device);
        ran = declared &&
              executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{})
                  .has_value() &&
              device.wait_idle().has_value();
        executor.release();
        ran = device.end_frame().has_value() && ran;
    }
    out.raw.assign(scene.depth.size(), Vec4{});
    out.filtered.assign(scene.depth.size(), Vec4{});
    ran = ran && pass.read_back_raw(Span<Vec4>(out.raw.data(), out.raw.size())).has_value() &&
          pass.read_back(Span<Vec4>(out.filtered.data(), out.filtered.size())).has_value();
    for (UploadedTexture& input : inputs) {
        device.destroy_texture(input.texture);
        device.destroy_buffer(input.staging);
    }
    pass.destroy();
    return ran;
}

// --- The frame --------------------------------------------------------------------------------

/// What a frame case configures, and the hooks that do it.
struct FrameCase {
    FrameScene* scene = nullptr;
    AmbientOcclusionPass* pass = nullptr;
    /// THE SETTING: `PostChainConfig::ambient_occlusion`.
    bool enabled = false;
    /// Whether the pass is attached to the frame at all. (d) compares attached-and-off against
    /// not attached.
    bool attached = false;
    /// (c): the ambient term zeroed, so every pixel is direct light alone.
    bool zero_ambient = false;
    /// (e) through the temporal resolve: the ambient term scaled up so it, and not the sun, is what
    /// an occluded pixel shows — the stage's motion is then visible in the resolved colour.
    f32 ambient_scale = 1.0F;
    /// The jitter the scene's temporal anti-aliasing samples with. (e) turns it off: with it, the
    /// depth moves a sub-pixel every frame, and the term follows the depth it is given.
    bool jitter = true;
};

void configure_case(rendering::assembly::AssemblyDescription& description, void* user) noexcept {
    const auto* frame = static_cast<const FrameCase*>(user);
    // PINNED, so two scenes rendering their first frames draw the same sub-pixel sample.
    description.pin_jitter = true;
    description.post.ambient_occlusion = frame->enabled;
    if (!frame->jitter) {
        description.temporal.jitter.spread = 0.0F;
    }
}

/// THE CONTACT THE FRAME CASES MEASURE. The pipeline scene's one box that meets the floor is
/// hidden behind another from this camera, and the nearest box stands on nothing in view; so the
/// box in front is set down ON the slab, a little farther back, where its foot and both of the
/// inner corners it makes with the floor are in the picture. Every other box is where the pipeline
/// suites put it.
constexpr u32 kContactBox = 8;
constexpr f32 kFloorTop = -1.9F + 0.125F;

void place_box(u32 which, Vec3& centre, f32& half, void* /*user*/) noexcept {
    if (which != kContactBox) {
        return;
    }
    half = 0.6F;
    centre = Vec3{-0.3F, kFloorTop + half, -5.6F};
}

Status before_assemble(rendering::RenderGraph& graph, rendering::assembly::AssemblyView& view,
                       rendering::assembly::FrameSinks& sinks, void* user) noexcept {
    auto* frame = static_cast<FrameCase*>(user);
    if (!frame->attached) {
        return ok();
    }
    rendering::occlusion::GtaoView occlusion;
    occlusion.projection = frame->scene->projection();
    occlusion.relative_to_view = frame->scene->view();
    occlusion.width = kWidth;
    occlusion.height = kHeight;
    if (Status set = frame->pass->set_view(occlusion); !set) {
        return set;
    }
    view.ambient_occlusion = frame->pass->import_target(graph);
    sinks.ambient_occlusion = frame->pass->stage();
    return ok();
}

Status before_upload(rendering::pipeline::FrameUpload& upload, void* user) noexcept {
    auto* frame = static_cast<FrameCase*>(user);
    if (frame->attached) {
        // THE JITTER IS KNOWN ONCE THE FRAME IS ASSEMBLED, and the search reads its constants when
        // it records, so the view is set again here with the jitter the prepass is drawn with.
        rendering::occlusion::GtaoView occlusion;
        occlusion.projection = frame->scene->projection();
        occlusion.relative_to_view = frame->scene->view();
        occlusion.width = kWidth;
        occlusion.height = kHeight;
        occlusion.jitter = Vec2{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]};
        if (Status set = frame->pass->set_view(occlusion); !set) {
            return set;
        }
    }
    for (u32 channel = 0; channel < 3; ++channel) {
        upload.view.ambient_and_occlusion[channel] *= frame->ambient_scale;
    }
    if (frame->zero_ambient) {
        upload.view.ambient_and_occlusion[0] = 0.0F;
        upload.view.ambient_and_occlusion[1] = 0.0F;
        upload.view.ambient_and_occlusion[2] = 0.0F;
    }
    if (!frame->attached || !frame->enabled) {
        return ok();
    }
    const rendering::pipeline::MaterialTextureSlot slot{kOcclusionSlot, frame->pass->target_view()};
    if (Status bound = frame->scene->set_frame_textures(
            Span<const rendering::pipeline::MaterialTextureSlot>(&slot, 1));
        !bound) {
        return bound;
    }
    rendering::occlusion::write_occlusion_control(kOcclusionSlot, frame->pass->settings().shared,
                                                  upload.view.occlusion_control);
    return ok();
}

/// One scene, built with a case's hooks, and the frames it rendered.
class FrameRun {
public:
    FrameRun(DeviceFixture& fixture, bool enabled, bool attached, bool zero_ambient,
             bool jitter = true, f32 ambient_scale = 1.0F)
        : scene_(allocator()) {
        frame_.scene = &scene_;
        frame_.pass = &pass_;
        frame_.enabled = enabled;
        frame_.attached = attached;
        frame_.zero_ambient = zero_ambient;
        frame_.ambient_scale = ambient_scale;
        frame_.jitter = jitter;
        if (attached) {
            AmbientOcclusionPassDescription description;
            description.width = kWidth;
            description.height = kHeight;
            description.readback = true;
            ready_ = pass_.create(allocator(), fixture.device(), description).has_value();
            rendering::occlusion::GtaoSettings settings;
            settings.shared.radius = kFrameRadius;
            pass_.set_settings(settings);
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
            std::fprintf(stderr, "occlusion frame: build failed: %s\n", built.error().message);
        }
        ready_ = built.has_value() && (ready_ || !attached);
        scene_.set_read_back(true);
    }

    [[nodiscard]] bool render() {
        if (!ready_) {
            return false;
        }
        const Status rendered = scene_.render(RecordMode::Callbacks, report_);
        if (!rendered) {
            std::fprintf(stderr, "occlusion frame: render failed: %s\n", rendered.error().message);
            return false;
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        if (frame_.attached && frame_.enabled) {
            term_.assign(static_cast<usize>(kWidth) * kHeight, Vec4{});
            return pass_.read_back(Span<Vec4>(term_.data(), term_.size())).has_value();
        }
        return true;
    }

    [[nodiscard]] const std::vector<u32>& pixels() const { return pixels_; }
    [[nodiscard]] const std::vector<Vec4>& term() const { return term_; }
    [[nodiscard]] const rendering::assembly::AssemblyReport& report() const { return report_; }
    [[nodiscard]] FrameScene& scene() { return scene_; }

private:
    FrameScene scene_;
    AmbientOcclusionPass pass_;
    FrameCase frame_;
    rendering::assembly::AssemblyReport report_{};
    std::vector<u32> pixels_;
    std::vector<Vec4> term_;
    bool ready_ = false;
};

/// The ray through a pixel centre, at unit view depth. The scene's camera is at the origin looking
/// down -Z with an identity view, so camera-relative and view space are the same.
[[nodiscard]] Vec3 pixel_ray(const Mat4& projection, u32 x, u32 y) noexcept {
    const f32 ndc_x = (((static_cast<f32>(x) + 0.5F) / kWidth) * 2.0F) - 1.0F;
    const f32 ndc_y = 1.0F - (((static_cast<f32>(y) + 0.5F) / kHeight) * 2.0F);
    return Vec3{ndc_x / projection.columns[0].x, ndc_y / projection.columns[1].y, -1.0F};
}

/// Slab test: the ray's entry parameter into a box, or infinity.
[[nodiscard]] f32 ray_box(Vec3 ray, const Aabb& box) noexcept {
    f32 near = 0.0F;
    f32 far = INFINITY;
    const f32 origin[3] = {0.0F, 0.0F, 0.0F};
    const f32 direction[3] = {ray.x, ray.y, ray.z};
    const f32 low[3] = {box.min.x, box.min.y, box.min.z};
    const f32 high[3] = {box.max.x, box.max.y, box.max.z};
    for (u32 axis = 0; axis < 3; ++axis) {
        if (std::fabs(direction[axis]) < 1.0e-9F) {
            if (origin[axis] < low[axis] || origin[axis] > high[axis]) {
                return INFINITY;
            }
            continue;
        }
        f32 t0 = (low[axis] - origin[axis]) / direction[axis];
        f32 t1 = (high[axis] - origin[axis]) / direction[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        near = std::max(near, t0);
        far = std::min(far, t1);
        if (near > far) {
            return INFINITY;
        }
    }
    return near;
}

[[nodiscard]] f32 box_distance(Vec3 point, const Aabb& box) noexcept {
    const f32 dx = std::max({box.min.x - point.x, 0.0F, point.x - box.max.x});
    const f32 dy = std::max({box.min.y - point.y, 0.0F, point.y - box.max.y});
    const f32 dz = std::max({box.min.z - point.z, 0.0F, point.z - box.max.z});
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

/// What a pixel's ray hits first: the index of the box, or -1.
[[nodiscard]] i32 first_hit(Span<const Aabb> boxes, Vec3 ray, f32& t) noexcept {
    i32 hit = -1;
    t = INFINITY;
    for (usize index = 0; index < boxes.size(); ++index) {
        const f32 entry = ray_box(ray, boxes[index]);
        if (entry < t) {
            t = entry;
            hit = static_cast<i32>(index);
        }
    }
    return hit;
}

[[nodiscard]] i32 channel(u32 texel, u32 which) noexcept {
    return static_cast<i32>((texel >> (which * 8U)) & 0xFFU);
}

[[nodiscard]] i32 brightness(u32 texel) noexcept {
    return channel(texel, 0) + channel(texel, 1) + channel(texel, 2);
}

[[nodiscard]] i32 largest_channel_difference(u32 a, u32 b) noexcept {
    i32 largest = 0;
    for (u32 which = 0; which < 4; ++which) {
        largest = std::max(largest, std::abs(channel(a, which) - channel(b, which)));
    }
    return largest;
}

[[nodiscard]] usize differing(const std::vector<u32>& a, const std::vector<u32>& b) noexcept {
    usize count = 0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        count += static_cast<usize>(a[index] != b[index]);
    }
    return count + (a.size() > b.size() ? a.size() - b.size() : b.size() - a.size());
}

}  // namespace

CY_TEST_CASE(
    "the horizon search on the device is the host reference's, and the cascade is the "
    "shared denoiser's") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    const CornerScene scene = make_corner_scene(true);
    PassRun run;
    CY_REQUIRE(run_pass(fixture, scene, run));

    // The host reference, over the same inputs.
    rendering::occlusion::GtaoSettings settings;
    settings.shared.radius = 1.0F;
    const auto constants = rendering::occlusion::make_gtao_constants(settings, scene.view);
    CY_REQUIRE(constants.has_value());
    rendering::occlusion::GtaoInputs inputs;
    inputs.width = kSceneWidth;
    inputs.height = kSceneHeight;
    inputs.depth = Span<const f32>(scene.depth.data(), scene.depth.size());
    inputs.normals = Span<const Vec2>(scene.normals.data(), scene.normals.size());
    std::vector<Vec4> reference(scene.depth.size());
    CY_REQUIRE(rendering::occlusion::gtao_reference(inputs, *constants,
                                                    Span<Vec4>(reference.data(), reference.size()))
                   .has_value());
    f32 worst_search = 0.0F;
    f32 occluded = 0.0F;
    for (usize pixel = 0; pixel < reference.size(); ++pixel) {
        worst_search = std::max(worst_search, std::fabs(run.raw[pixel].w - reference[pixel].w));
        worst_search = std::max(worst_search, std::fabs(run.raw[pixel].y - reference[pixel].y));
        occluded += run.raw[pixel].w < 0.9F ? 1.0F : 0.0F;
    }
    std::fprintf(stderr, "horizon search: worst device/host difference %.3g, %g pixels below 0.9\n",
                 static_cast<double>(worst_search), static_cast<double>(occluded));
    CY_CHECK_LE(worst_search, kSearchTolerance);
    CY_CHECK_GT(occluded, 200.0F);

    // The shared denoiser, over the DEVICE's raw term, with the ambient occlusion configuration.
    rendering::denoise::Denoiser denoiser;
    CY_REQUIRE(denoiser.resize(kSceneWidth, kSceneHeight).has_value());
    std::vector<Vec3> values(reference.size());
    std::vector<f32> depth(reference.size());
    std::vector<Vec3> normals(reference.size());
    for (usize pixel = 0; pixel < reference.size(); ++pixel) {
        values[pixel] = Vec3{run.raw[pixel].w, run.raw[pixel].x, run.raw[pixel].y};
        depth[pixel] = scene.depth[pixel] > 0.0F ? rendering::occlusion::view_depth(
                                                       constants->projection, scene.depth[pixel])
                                                 : 0.0F;
        normals[pixel] = rendering::occlusion::decode_octahedral(scene.normals[pixel]);
    }
    rendering::denoise::NoisySignal noisy;
    noisy.values = Span<const Vec3>(values.data(), values.size());
    rendering::denoise::GuidanceBuffers guidance;
    guidance.width = kSceneWidth;
    guidance.height = kSceneHeight;
    guidance.depth = Span<const f32>(depth.data(), depth.size());
    guidance.normal = Span<const Vec3>(normals.data(), normals.size());
    const auto denoised = denoiser.denoise(rendering::denoise::SignalKind::AmbientOcclusion, noisy,
                                           guidance, rendering::denoise::HistoryGuidance{});
    CY_REQUIRE(denoised.has_value());
    f32 worst_filter = 0.0F;
    f32 worst_bent = 0.0F;
    f64 total_filter = 0.0;
    u32 beyond = 0;
    f32 changed = 0.0F;
    for (usize pixel = 0; pixel < reference.size(); ++pixel) {
        const f32 visibility = std::fabs(run.filtered[pixel].w - (*denoised)[pixel].x);
        const f32 bent = std::fabs(run.filtered[pixel].x - (*denoised)[pixel].y);
        worst_filter = std::max(worst_filter, visibility);
        worst_bent = std::max(worst_bent, bent);
        total_filter += static_cast<f64>(visibility);
        beyond += visibility > kFilterTolerance ? 1U : 0U;
        changed = std::max(changed, std::fabs(run.filtered[pixel].w - run.raw[pixel].w));
    }
    const f64 mean_filter = total_filter / static_cast<f64>(reference.size());
    std::fprintf(stderr,
                 "cascade: device/denoiser visibility difference mean %.3g, worst %.3g, %u "
                 "pixel(s) beyond %.3g; bent normal worst %.3g; the filter moved a pixel by up to "
                 "%.3g\n",
                 mean_filter, static_cast<double>(worst_filter), beyond,
                 static_cast<double>(kFilterTolerance), static_cast<double>(worst_bent),
                 static_cast<double>(changed));
    CY_CHECK_LE(worst_filter, kFilterTolerance);
    CY_CHECK_LE(worst_bent, kBentTolerance);
    // The control: the cascade did something, or the comparison above would pass on a copy.
    CY_CHECK_GT(changed, 0.02F);
    CY_CHECK_EQ(
        denoiser.diagnostics(rendering::denoise::SignalKind::AmbientOcclusion).passes_applied,
        rendering::occlusion::filter_pass_count());
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(a) a contact is darker with ambient occlusion on, (b) open floor is unchanged") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameRun off(fixture, false, false, false);
    FrameRun on(fixture, true, true, false);
    CY_REQUIRE(off.render());
    CY_REQUIRE(on.render());
    save("ambient-occlusion-off.png", Span<const u32>(off.pixels().data(), off.pixels().size()));
    save("ambient-occlusion-on.png", Span<const u32>(on.pixels().data(), on.pixels().size()));

    const Span<const Aabb> boxes = on.scene().boxes();
    const Mat4& projection = on.scene().projection();
    const f32 floor_top = boxes[0].max.y;
    // (a) THE CONTACT, found the way the horizon search finds it: a floor pixel with a box's
    // surface within `kContact` metres of it in the next few pixels up the screen — the place a
    // box meets, or nearly meets, the floor in the picture.
    constexpr f32 kContact = 0.15F;
    constexpr u32 kContactReach = 4;
    i32 darker_sum = 0;
    u32 contacts = 0;
    f32 term_sum = 0.0F;
    // (b) OPEN FLOOR: floor pixels farther than 1.5 radii from every box.
    u32 open = 0;
    i32 open_worst = 0;
    f32 open_lowest = 1.0F;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const Vec3 ray = pixel_ray(projection, x, y);
            f32 t = 0.0F;
            if (first_hit(boxes, ray, t) != 0) {
                continue;
            }
            const Vec3 point = ray * t;
            if (std::fabs(point.y - floor_top) > 1.0e-3F) {
                continue;  // the slab's side, not its top
            }
            f32 nearest = INFINITY;
            for (u32 up = 1; up <= kContactReach && up <= y; ++up) {
                const Vec3 above = pixel_ray(projection, x, y - up);
                f32 hit_t = 0.0F;
                if (first_hit(boxes, above, hit_t) > 0) {
                    const Vec3 surface = above * hit_t;
                    const Vec3 gap{surface.x - point.x, surface.y - point.y, surface.z - point.z};
                    nearest = std::min(nearest, std::sqrt(dot(gap, gap)));
                }
            }
            f32 clearance = INFINITY;
            for (usize index = 1; index < boxes.size(); ++index) {
                clearance = std::min(clearance, box_distance(point, boxes[index]));
            }
            const usize pixel = (static_cast<usize>(y) * kWidth) + x;
            if (nearest < kContact) {
                darker_sum += brightness(off.pixels()[pixel]) - brightness(on.pixels()[pixel]);
                term_sum += on.term()[pixel].w;
                ++contacts;
            }
            // Two pixels in from the slab's own edges, where the prepass normal is the side's.
            const bool inside = point.x > boxes[0].min.x + kFrameRadius &&
                                point.x < boxes[0].max.x - kFrameRadius &&
                                point.z > boxes[0].min.z + kFrameRadius &&
                                point.z < boxes[0].max.z - kFrameRadius;
            if (inside && clearance > 1.5F * kFrameRadius) {
                ++open;
                open_worst = std::max(open_worst, largest_channel_difference(off.pixels()[pixel],
                                                                             on.pixels()[pixel]));
                open_lowest = std::min(open_lowest, on.term()[pixel].w);
            }
        }
    }
    std::fprintf(
        stderr,
        "(a) %u contact pixels, mean term %.3f, mean darkening %.2f steps (of 765)\n"
        "(b) %u open floor pixels, lowest term %.5f, worst channel change %d\n",
        contacts, contacts > 0 ? static_cast<double>(term_sum / static_cast<f32>(contacts)) : 1.0,
        contacts > 0 ? static_cast<double>(darker_sum) / static_cast<double>(contacts) : 0.0, open,
        static_cast<double>(open_lowest), open_worst);
    CY_REQUIRE((contacts) > (20U));
    // MEASURED: see the line above. The term at a contact is well below one and the frame is
    // darker there by several 8-bit steps summed over the three channels.
    CY_CHECK_LT(term_sum / static_cast<f32>(contacts), 0.8F);
    CY_CHECK_GT(darker_sum, static_cast<i32>(contacts) * 3);
    CY_REQUIRE((open) > (1000U));
    // (b)'s TOLERANCES: the frame moves by at most one 8-bit step in any channel (MEASURED: zero),
    // and the term stays within 5e-3 of one (MEASURED: 1.5e-3, three half-precision steps below
    // one). The term is not exactly one because the cascade reaches fourteen pixels and a floor
    // pixel 1.5 radii from a box can still take a trace of that box's contact through it.
    CY_CHECK_GE(open_lowest, 1.0F - 5.0e-3F);
    CY_CHECK_LE(open_worst, 1);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) direct light alone is unaffected by ambient occlusion, byte for byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameRun off(fixture, false, false, true);
    FrameRun on(fixture, true, true, true);
    CY_REQUIRE(off.render());
    CY_REQUIRE(on.render());
    // The control: the term is not trivially one, so an unchanged frame is AO leaving direct light
    // alone rather than AO doing nothing.
    f32 lowest = 1.0F;
    for (const Vec4& texel : on.term()) {
        lowest = std::min(lowest, texel.w);
    }
    CY_CHECK_LT(lowest, 0.8F);
    const usize changed = differing(off.pixels(), on.pixels());
    std::fprintf(stderr,
                 "(c) %zu of %u pixels differ with the ambient term zeroed; lowest term %.3f\n",
                 changed, kWidth * kHeight, static_cast<double>(lowest));
    CY_CHECK_EQ(changed, usize{0});
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) ambient occlusion off is the frame without it, byte for byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameRun absent(fixture, false, false, false);
    FrameRun off(fixture, false, true, false);
    CY_REQUIRE(absent.render());
    CY_REQUIRE(off.render());
    CY_CHECK_EQ(off.report().passes_declared, absent.report().passes_declared);
    CY_CHECK_EQ(off.report().execution.passes_recorded, absent.report().execution.passes_recorded);
    CY_CHECK_EQ(off.report().post_stages, absent.report().post_stages);
    CY_CHECK_EQ(off.scene().recorded().passes, absent.scene().recorded().passes);
    const usize changed = differing(absent.pixels(), off.pixels());
    std::fprintf(stderr, "(d) %zu pixels differ between AO off and AO absent\n", changed);
    CY_CHECK_EQ(changed, usize{0});
    // AND ABSENT IS THE FRAME AS IT WAS. The two frames above share this branch's frame shader, so
    // a change to the path every frame takes without the stage would move both and still compare
    // equal. The reference pins that path to the frame the shader drew before the stage existed.
    check_against_before(absent.pixels());

    // The control: switched on, the same scene declares the stage and draws a different frame.
    FrameRun on(fixture, true, true, false);
    CY_REQUIRE(on.render());
    std::fprintf(stderr,
                 "(d) stages declared: absent %u, off %u, on %u; graph passes recorded: "
                 "absent %u, off %u, on %u\n",
                 absent.report().passes_declared, off.report().passes_declared,
                 on.report().passes_declared, absent.report().execution.passes_recorded,
                 off.report().execution.passes_recorded, on.report().execution.passes_recorded);
    CY_CHECK_GT(on.report().passes_declared, absent.report().passes_declared);
    CY_CHECK_GT(differing(absent.pixels(), on.pixels()), usize{100});
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(e) the term is still across frames of a still view") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameRun on(fixture, true, true, false, false);
    CY_REQUIRE(on.render());
    std::vector<Vec4> previous = on.term();
    std::vector<u32> previous_pixels = on.pixels();
    f32 worst = 0.0F;
    i32 worst_pixel = 0;
    for (u32 frame = 1; frame < 4; ++frame) {
        CY_REQUIRE(on.render());
        for (usize texel = 0; texel < previous.size(); ++texel) {
            worst = std::max(worst, std::fabs(on.term()[texel].w - previous[texel].w));
        }
        for (usize texel = 0; texel < previous_pixels.size(); ++texel) {
            worst_pixel = std::max(worst_pixel, largest_channel_difference(on.pixels()[texel],
                                                                           previous_pixels[texel]));
        }
        previous = on.term();
        previous_pixels = on.pixels();
    }
    std::fprintf(stderr, "(e) worst frame-to-frame change: term %.3g, pixel %d steps\n",
                 static_cast<double>(worst), worst_pixel);
    CY_CHECK_LE(worst, kStableTolerance);
    CY_CHECK_LE(worst_pixel, 1);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

namespace {

/// What the stage adds to a resolved frame's frame-to-frame change, measured where the frame
/// without the stage did not move at all — the jitter does not change what those pixels see, so
/// anything that moves there with the stage is the stage's — and where the term is occluded, the
/// only place a search's noise can show: an open surface is exactly one whatever the noise.
struct Crawl {
    /// Pixel-frames still without the stage and occluded (term below `kCrawlOccluded`) with it.
    u64 occluded = 0;
    /// The mean absolute 8-bit change of those pixel-frames' channels with the stage on.
    f64 pixel = 0.0;
    /// The mean frame-to-frame change of the term's visibility over the same pixel-frames.
    f64 term = 0.0;
};

/// Render both scenes `settle` frames so the temporal history is full, then `frames` more.
[[nodiscard]] bool measure_crawl(FrameRun& off, FrameRun& on, u32 settle, u32 frames, Crawl& out) {
    for (u32 frame = 0; frame < settle; ++frame) {
        if (!off.render() || !on.render()) {
            return false;
        }
    }
    std::vector<u32> off_previous = off.pixels();
    std::vector<u32> on_previous = on.pixels();
    std::vector<Vec4> term_previous = on.term();
    u64 pixel_total = 0;
    f64 term_total = 0.0;
    for (u32 frame = 0; frame < frames; ++frame) {
        if (!off.render() || !on.render()) {
            return false;
        }
        for (usize texel = 0; texel < off_previous.size(); ++texel) {
            if (off.pixels()[texel] != off_previous[texel] ||
                on.term()[texel].w >= kCrawlOccluded) {
                continue;
            }
            ++out.occluded;
            for (u32 which = 0; which < 3; ++which) {
                pixel_total += static_cast<u64>(std::abs(channel(on.pixels()[texel], which) -
                                                         channel(on_previous[texel], which)));
            }
            term_total += static_cast<f64>(std::fabs(on.term()[texel].w - term_previous[texel].w));
        }
        off_previous = off.pixels();
        on_previous = on.pixels();
        term_previous = on.term();
    }
    const f64 count = static_cast<f64>(std::max<u64>(out.occluded, 1U));
    out.pixel = static_cast<f64>(pixel_total) / (3.0 * count);
    out.term = term_total / count;
    return true;
}

}  // namespace

CY_TEST_CASE("(e) with temporal anti-aliasing, the term adds no crawl to the resolved frame") {
    // THE SAME PROPERTY THROUGH THE WHOLE CHAIN. With the scene's temporal anti-aliasing on, the
    // jitter moves the depth a sub-pixel every frame and the resolved frame moves with it at every
    // edge. So the stage is measured where the frame WITHOUT it does not move at all — the same
    // jitter, the same frames: there, anything the frame with it does is the stage's. A search
    // whose noise pattern changed per frame, or a filter that let the dither through, moves those
    // pixels; the term following the geometry does not.
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    constexpr u32 kSettle = 8;
    constexpr u32 kFrames = 8;
    FrameRun off(fixture, false, false, false, true, kCrawlAmbient);
    FrameRun on(fixture, true, true, false, true, kCrawlAmbient);
    Crawl crawl;
    CY_REQUIRE(measure_crawl(off, on, kSettle, kFrames, crawl));
    std::fprintf(stderr,
                 "(e) over %u frames, %llu occluded pixel-frames still without the stage: with it, "
                 "mean change %.4f steps per channel, term mean change %.3g\n",
                 kFrames, static_cast<unsigned long long>(crawl.occluded), crawl.pixel, crawl.term);
    // The control: there are occluded pixels to measure.
    CY_REQUIRE(crawl.occluded > 1000U);
    CY_CHECK_LE(crawl.term, kCrawlTerm);
    CY_CHECK_LE(crawl.pixel, kCrawlPixel);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
