// SPDX-License-Identifier: MIT
// Decals in the forward frame, on a Vulkan device with validation and synchronisation validation
// on. `render.decals`.
//
// ================================================================================================
// THE SCENE, AND WHY EVERY ANSWER IS READ EXACTLY
// ================================================================================================
//
// `pipeline_test::FrameScene` — the one scene the pipeline suites render — with a directional
// shadow map (the scene has none of its own), one box resting on the floor and the rest of the ring
// moved out of the view and out of the shadow volume. The sun travels down, to the right and toward
// the camera, so the box's shadow falls on floor the camera sees.
//
// The frame is deterministic, so the cases compare EQUALITY rather than thresholds: a decal that
// changes no pixel leaves the frame byte-identical, a pixel the sun does not reach is
// byte-identical to the same frame with the sun switched off, and two frames that apply the same
// decals in the same order are the same bytes. Where the geometry decides a question — which floor
// point a pixel sees, and whether it is inside a decal's box — the answer is traced on the
// processor from the scene's own boxes and camera.
//
//   (a) no decal table is the frame as it was, byte for byte against a committed reference the
//       pre-change frame shader drew;
//   (b) a decal whose layer is none, one restricted to channels the receivers are not in, and one
//       faded out by distance each leave the frame byte-identical to (a) — and the same decal in
//       every channel does not;
//   (c) a decal changes only pixels inside its projected box, and every pixel well inside it;
//   (d) a decal is LIT: where the receiver is in umbra the decal is exactly what it is with the sun
//       off, where the receiver is lit it is not, and it is darker in the shadow than out of it;
//   (e) a steep receiver fades: a box's vertical face inside the decal's box is untouched while the
//       floor around it is decalled;
//   (f) order is deterministic: a later decal covers an earlier one where they overlap, swapping
//       their sort orders swaps which covers, and handing them over in another array order draws
//       the same bytes;
//   (g) the lists are clusters: the frame through the cluster lists, through the table's copy of
//       them, and walking every decal with no lists at all draw the same bytes, and the lists
//       really cull;
//   (h) twenty thousand decals draw with the same number of draw calls as none;
//   (i) relief tilts the normal and nothing else: a normal-only decal changes the shading inside
//       its box, and with zero relief it changes nothing.

#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/decals/decal_table.h>
#include <cy/rendering/lighting/decals.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

using namespace cy;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using cy::rendering::DecalInstance;
using cy::rendering::decals::DecalMaterial;
using cy::rendering::decals::DecalShape;
using cy::rendering::decals::DecalTableTexture;

namespace {

/// Free slots of the frame's set 0 texture table; the scene binds no material texture of its own.
constexpr u32 kShadowSlot = 121;
constexpr u32 kDecalSlot = 123;
/// The shadow volume: an orthographic box `2 * kShadowRadius` metres across, centred on the floor.
constexpr f32 kShadowRadius = 9.0F;
constexpr f32 kShadowNear = 0.1F;
constexpr f32 kShadowFar = kShadowRadius * 4.0F;
const Vec3 kShadowCentre{0.0F, -1.8F, -7.0F};
constexpr u32 kShadowExtent = 2048;

/// FrameScene's floor slab: centre y -1.9, half-height 0.125.
constexpr f32 kFloorTop = -1.9F + 0.125F;
constexpr f32 kBoxHalf = 0.45F;
const Vec3 kBoxCentre{-1.0F, kFloorTop + kBoxHalf, -6.0F};

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
        description.application_name = "cy_test_render_decals";
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

/// `references/decals_off.png`: (a)'s frame — the shadow map on, no decal table — rendered by the
/// frame shader as it was before decals: the suite run once with `CY_RENDER_UPDATE_GOLDEN=1`
/// against main's `frame_spirv.h`, before the shader was regenerated. The change only appended
/// `decalControl` to the frame block, so the old shader reads the same frame data.
const char* before_reference_path() noexcept {
    static char storage[1024];
    (void)std::snprintf(storage, sizeof(storage), "%s/references/decals_off.png",
                        CY_DECALS_TEST_DIR);
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

// --- Decals ------------------------------------------------------------------------------------

/// A floor decal: projected straight down (its +Z is up), `half_x` by `half_z` metres, `depth`
/// metres either side of the floor's top.
[[nodiscard]] DecalInstance floor_decal(Vec3 centre, f32 half_x, f32 half_z, f32 depth,
                                        u32 material) noexcept {
    DecalInstance decal;
    decal.center = centre;
    decal.axis_x = Vec3{1.0F, 0.0F, 0.0F};
    decal.axis_y = Vec3{0.0F, 0.0F, -1.0F};
    decal.axis_z = Vec3{0.0F, 1.0F, 0.0F};
    decal.half_extent = Vec3{half_x, half_z, depth};
    decal.material_index = material;
    // Albedo only unless a case says otherwise, so what changes is what the receiver's light loop
    // does with a different albedo.
    decal.weights.albedo = 1.0F;
    decal.weights.normal = 0.0F;
    decal.weights.roughness = 0.0F;
    decal.weights.metallic = 0.0F;
    decal.weights.emission = 0.0F;
    decal.fade_start = 40.0F;
    decal.fade_end = 60.0F;
    return decal;
}

/// Three materials: a red box, a blue box and a black splat. Linear albedo.
[[nodiscard]] std::vector<DecalMaterial> materials() {
    std::vector<DecalMaterial> out(3);
    out[0].albedo = Vec3{0.8F, 0.05F, 0.05F};
    out[1].albedo = Vec3{0.05F, 0.1F, 0.8F};
    out[2].albedo = Vec3{0.03F, 0.03F, 0.03F};
    out[2].roughness = 0.9F;
    out[2].shape = DecalShape::Splat;
    out[2].shape_a = 5.0F;
    out[2].shape_b = 0.35F;
    out[2].relief_metres = 0.004F;
    return out;
}

/// Give each decal the id a budget would: its index plus one, in spawn order.
void number(std::vector<DecalInstance>& decals) noexcept {
    for (usize index = 0; index < decals.size(); ++index) {
        decals[index].id = static_cast<u64>(index) + 1U;
    }
}

// --- The frame ---------------------------------------------------------------------------------

struct FrameCase {
    FrameScene* scene = nullptr;
    DecalTableTexture* table = nullptr;
    rhi::TextureHandle shadow_color;
    rhi::TextureHandle shadow_depth;
    rhi::TextureViewHandle shadow_view;
    bool shadowed = true;
    bool dark = false;
    /// Bind the table and name it in the view block. Off is the frame with no decals at all.
    bool bind = false;
    u32 flags = 0;
    std::vector<DecalInstance> decals;
    rendering::FrameResourceRead reads[1] = {};
    std::vector<rendering::GpuLight> lights;
};

void configure_case(rendering::assembly::AssemblyDescription& description,
                    void* /*user*/) noexcept {
    description.pin_jitter = true;
}

/// One box resting on the floor, and the rest of the ring out of the view and the shadow volume.
void place_box(u32 which, Vec3& centre, f32& half, void* /*user*/) noexcept {
    if (which == 1U) {
        centre = kBoxCentre;
        half = kBoxHalf;
        return;
    }
    centre = Vec3{static_cast<f32>(which) * 3.0F, 0.0F, 60.0F};
    half = 0.3F;
}

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
    view.decals = Span<const DecalInstance>(frame->decals.data(), frame->decals.size());
    rendering::TextureRequest request;
    request.name = "decal test shadow map";
    request.format = rhi::Format::R32Sfloat;
    request.width = kShadowExtent;
    request.height = kShadowExtent;
    view.shadow_color =
        graph.import_texture(request, frame->shadow_color, rhi::ImageUse::Undefined);
    request.name = "decal test shadow depth";
    request.format = rhi::Format::D32Sfloat;
    view.shadow_depth =
        graph.import_texture(request, frame->shadow_depth, rhi::ImageUse::Undefined);
    rendering::pipeline::FrameRecorder& recorder = frame->scene->recorder();
    recorder.set_shadow_targets(view.shadow_color, view.shadow_depth, kShadowExtent);
    const auto shadow = static_cast<usize>(rendering::FramePassKind::Shadow);
    sinks.passes[shadow] = recorder.sinks().passes[shadow];
    const auto opaque = static_cast<usize>(rendering::FramePassKind::Opaque);
    frame->reads[0] =
        rendering::FrameResourceRead{view.shadow_color, rhi::Access::FragmentSampledRead};
    sinks.passes[opaque].reads = Span<const rendering::FrameResourceRead>(frame->reads, 1);
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
    }
    upload.lights = Span<const rendering::GpuLight>(frame->lights.data(), frame->lights.size());
    const Mat4 to_clip = shadow_to_clip();
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            upload.view.shadow_to_clip[(row * 4U) + column] = to_clip.at(row, column);
        }
    }
    upload.view.shadow_control[0] = kShadowSlot;
    upload.view.shadow_control[1] = sun;
    upload.view.shadow_control[2] = kShadowExtent;
    upload.view.shadow_control[3] = frame->shadowed ? 1U : 0U;

    rendering::pipeline::MaterialTextureSlot slots[2] = {
        {kShadowSlot, frame->shadow_view},
        {kDecalSlot, frame->table->view()},
    };
    const bool bound = frame->bind && frame->table->ready();
    if (bound) {
        rendering::decals::write_decal_frame(kDecalSlot, frame->table->rows(), frame->flags,
                                             upload.view);
    }
    return frame->scene->set_frame_textures(
        Span<const rendering::pipeline::MaterialTextureSlot>(slots, bound ? 2U : 1U));
}

/// One scene, built with a case's hooks, and the frame it rendered.
class FrameRun {
public:
    struct Options {
        bool shadowed = true;
        bool dark = false;
        bool bind = false;
        u32 flags = 0;
        std::vector<DecalInstance> decals;
        std::vector<DecalMaterial> materials = ::materials();
    };

    FrameRun(DeviceFixture& fixture, Options options)
        : device_(&fixture.device()), scene_(allocator()), options_(std::move(options)) {
        frame_.scene = &scene_;
        frame_.table = &table_;
        frame_.shadowed = options_.shadowed;
        frame_.dark = options_.dark;
        frame_.bind = options_.bind;
        frame_.flags = options_.flags;
        frame_.decals = options_.decals;
        table_.initialize(fixture.device(), allocator());
        ready_ = create_shadow_map();
        FrameSceneHooks hooks;
        hooks.user = &frame_;
        hooks.configure = &configure_case;
        hooks.before_assemble = &before_assemble;
        hooks.before_upload = &before_upload;
        hooks.place_box = &place_box;
        scene_.set_hooks(hooks);
        const Status built = scene_.build(fixture.device());
        if (!built) {
            std::fprintf(stderr, "decal frame: build failed: %s\n", built.error().message);
        }
        ready_ = ready_ && built.has_value();
        scene_.set_read_back(true);
    }

    ~FrameRun() {
        (void)device_->wait_idle();
        table_.shutdown();
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

    /// Pack and upload the table, then render. With `kDecalListsInTable` the lists are the
    /// assembly's, so the frame is assembled once to produce them — the assembly is deterministic,
    /// and the second assembly assigns exactly what the first did.
    [[nodiscard]] bool render() {
        if (!ready_) {
            return false;
        }
        if (options_.bind) {
            if (!upload_table(nullptr)) {
                return false;
            }
            if ((options_.flags & rendering::decals::kDecalListsInTable) != 0U) {
                if (!draw() || !upload_table(&scene_.assembly())) {
                    return false;
                }
            }
        }
        return draw();
    }

    [[nodiscard]] const std::vector<u32>& pixels() const { return pixels_; }
    [[nodiscard]] const rendering::assembly::AssemblyReport& report() const { return report_; }
    [[nodiscard]] FrameScene& scene() { return scene_; }

private:
    [[nodiscard]] bool draw() {
        const Status rendered = scene_.render(RecordMode::Callbacks, report_);
        if (!rendered) {
            std::fprintf(stderr, "decal frame: render failed: %s\n", rendered.error().message);
            return false;
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        return true;
    }

    [[nodiscard]] bool upload_table(const rendering::assembly::FrameAssembly* assembly) {
        std::vector<u32> order(frame_.decals.size());
        (void)rendering::decal_application_order(
            Span<const DecalInstance>(frame_.decals.data(), frame_.decals.size()),
            Span<u32>(order.data(), order.size()));
        rendering::decals::DecalTableInput input;
        input.decals = Span<const DecalInstance>(frame_.decals.data(), frame_.decals.size());
        input.order = Span<const u32>(order.data(), order.size());
        input.materials =
            Span<const DecalMaterial>(options_.materials.data(), options_.materials.size());
        input.origin = Vec3{0.0F, 0.0F, 0.0F};
        Expected<rendering::ClusterGrid, Error> grid = rendering::ClusterGrid{};
        if (assembly != nullptr) {
            const rendering::assembly::AssemblyDescription& description = assembly->description();
            grid = rendering::make_cluster_grid(description.clusters, description.width,
                                                description.height, description.near_plane,
                                                description.far_plane);
            if (!grid.has_value()) {
                return false;
            }
            input.clusters = &assembly->clusters();
            input.grid = *grid;
            input.view = scene_.view();
            input.width = kWidth;
            input.height = kHeight;
            // The ranks the assembly assigned and the ranks packed here must be one ordering.
            const Span<const u32> assigned = assembly->decal_order();
            if (assigned.size() != order.size() ||
                !std::equal(order.begin(), order.end(), assigned.begin())) {
                std::fprintf(stderr, "decal frame: the assembly ranked the decals differently\n");
                return false;
            }
        }
        Array<u32> words(allocator());
        const Status packed = rendering::decals::pack_decal_table(input, words);
        if (!packed) {
            std::fprintf(stderr, "decal frame: pack failed: %s\n", packed.error().message);
            return false;
        }
        const Status uploaded = table_.upload(words.span());
        if (!uploaded) {
            std::fprintf(stderr, "decal frame: upload failed: %s\n", uploaded.error().message);
        }
        return uploaded.has_value();
    }

    [[nodiscard]] bool create_shadow_map() {
        rhi::TextureDescription texture;
        texture.name = "decal test shadow map";
        texture.format = rhi::Format::R32Sfloat;
        texture.extent = rhi::Extent3D{kShadowExtent, kShadowExtent, 1};
        texture.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        auto color = device_->create_texture(texture);
        if (!color.has_value()) {
            return false;
        }
        frame_.shadow_color = *color;
        texture.name = "decal test shadow depth";
        texture.format = rhi::Format::D32Sfloat;
        texture.usage = rhi::TextureUsage::DepthStencilAttachment;
        auto depth = device_->create_texture(texture);
        if (!depth.has_value()) {
            return false;
        }
        frame_.shadow_depth = *depth;
        rhi::TextureViewDescription view;
        view.name = "decal test shadow map";
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
    DecalTableTexture table_;
    Options options_;
    FrameCase frame_;
    rendering::assembly::AssemblyReport report_{};
    std::vector<u32> pixels_;
    bool ready_ = false;
};

[[nodiscard]] bool render_frame(DeviceFixture& fixture, FrameRun::Options options,
                                std::vector<u32>& out,
                                rendering::assembly::AssemblyReport* report = nullptr) {
    FrameRun run(fixture, std::move(options));
    if (!run.render()) {
        return false;
    }
    out = run.pixels();
    if (report != nullptr) {
        *report = run.report();
    }
    return true;
}

// --- The geometry's answers ----------------------------------------------------------------------

[[nodiscard]] Vec3 pixel_ray(const Mat4& projection, u32 x, u32 y) noexcept {
    const f32 ndc_x = (((static_cast<f32>(x) + 0.5F) / kWidth) * 2.0F) - 1.0F;
    const f32 ndc_y = 1.0F - (((static_cast<f32>(y) + 0.5F) / kHeight) * 2.0F);
    return Vec3{ndc_x / projection.columns[0].x, ndc_y / projection.columns[1].y, -1.0F};
}

/// The entry distance of a ray into a box and the face it enters through, or infinity.
[[nodiscard]] f32 ray_box(Vec3 origin, Vec3 direction, const Aabb& box, Vec3& normal) noexcept {
    const f32 o[3] = {origin.x, origin.y, origin.z};
    const f32 d[3] = {direction.x, direction.y, direction.z};
    const f32 lo[3] = {box.min.x, box.min.y, box.min.z};
    const f32 hi[3] = {box.max.x, box.max.y, box.max.z};
    f32 enter = -INFINITY;
    f32 leave = INFINITY;
    u32 axis_in = 0;
    f32 sign_in = 0.0F;
    for (u32 axis = 0; axis < 3U; ++axis) {
        if (std::fabs(d[axis]) < 1.0e-12F) {
            if (o[axis] < lo[axis] || o[axis] > hi[axis]) {
                return INFINITY;
            }
            continue;
        }
        f32 t0 = (lo[axis] - o[axis]) / d[axis];
        f32 t1 = (hi[axis] - o[axis]) / d[axis];
        f32 sign = -1.0F;
        if (t0 > t1) {
            std::swap(t0, t1);
            sign = 1.0F;
        }
        if (t0 > enter) {
            enter = t0;
            axis_in = axis;
            sign_in = sign;
        }
        leave = std::min(leave, t1);
    }
    if (enter > leave || enter <= 0.0F) {
        return INFINITY;
    }
    normal = Vec3{axis_in == 0U ? sign_in : 0.0F, axis_in == 1U ? sign_in : 0.0F,
                  axis_in == 2U ? sign_in : 0.0F};
    return enter;
}

/// What one pixel sees: the point, the face normal, and which box (0 the floor slab), or -1.
struct Seen {
    i32 box = -1;
    Vec3 point{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 0.0F, 0.0F};
};

[[nodiscard]] Seen seen(FrameScene& scene, u32 x, u32 y) noexcept {
    const Span<const Aabb> boxes = scene.boxes();
    const Vec3 ray = pixel_ray(scene.projection(), x, y);
    Seen out;
    f32 nearest = INFINITY;
    for (usize index = 0; index < boxes.size(); ++index) {
        Vec3 normal{0.0F, 0.0F, 0.0F};
        const f32 t = ray_box(Vec3{0.0F, 0.0F, 0.0F}, ray, boxes[index], normal);
        if (t < nearest) {
            nearest = t;
            out.box = static_cast<i32>(index);
            out.point = ray * t;
            out.normal = normal;
        }
    }
    return out;
}

/// How far inside a decal's box a point is, in metres: positive inside, negative outside, the
/// smallest margin over the three axes.
[[nodiscard]] f32 inside_margin(const DecalInstance& decal, Vec3 point) noexcept {
    const Vec3 offset = point - decal.center;
    const f32 x = decal.half_extent.x - std::fabs(dot(offset, normalize(decal.axis_x)));
    const f32 y = decal.half_extent.y - std::fabs(dot(offset, normalize(decal.axis_y)));
    const f32 z = decal.half_extent.z - std::fabs(dot(offset, normalize(decal.axis_z)));
    return std::min({x, y, z});
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

/// A pixel far enough from every silhouette that a sub-pixel jitter cannot move it onto another
/// surface: its four neighbours see the same box.
[[nodiscard]] bool interior(FrameScene& scene, u32 x, u32 y, i32 box) noexcept {
    if (x == 0 || y == 0 || x + 1 >= kWidth || y + 1 >= kHeight) {
        return false;
    }
    return seen(scene, x - 1, y).box == box && seen(scene, x + 1, y).box == box &&
           seen(scene, x, y - 1).box == box && seen(scene, x, y + 1).box == box;
}

/// Pixel size on the floor at a point, in metres — generous, so an edge pixel's footprint is
/// always inside it.
constexpr f32 kEdgeMargin = 0.12F;

}  // namespace

CY_TEST_CASE("(a) no decal table is the frame before the change, byte for byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> off;
    CY_REQUIRE(render_frame(fixture, FrameRun::Options{}, off));
    save("decals-off.png", off);
    check_against_before(off);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) a decal layer of none, the wrong channels, or a full fade is the frame without") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> off;
    CY_REQUIRE(render_frame(fixture, FrameRun::Options{}, off));

    const DecalInstance base = floor_decal(Vec3{0.4F, kFloorTop, -5.0F}, 0.8F, 0.6F, 0.2F, 0);

    // The positive control first: in every channel, the decal changes the frame.
    FrameRun::Options all;
    all.bind = true;
    all.decals = {base};
    number(all.decals);
    std::vector<u32> decalled;
    rendering::assembly::AssemblyReport report{};
    CY_REQUIRE(render_frame(fixture, all, decalled, &report));
    const usize changed = differing(off, decalled);
    std::fprintf(stderr, "(b) a decal in every channel changes %zu pixels, %u assignments\n",
                 changed, report.decal_assignments);
    CY_CHECK_GT(changed, usize{500});
    CY_CHECK_GT(report.decal_assignments, 0U);

    // A layer of none: assigned to no cluster at all, and the frame is the frame without it.
    FrameRun::Options none = all;
    none.decals[0].channels = 0;
    std::vector<u32> nothing;
    CY_REQUIRE(render_frame(fixture, none, nothing, &report));
    std::fprintf(stderr, "(b) layer none: %zu pixels differ, %u assignments\n",
                 differing(off, nothing), report.decal_assignments);
    CY_CHECK_EQ(report.decals, 1U);
    CY_CHECK_EQ(report.decal_assignments, 0U);
    CY_CHECK_EQ(differing(off, nothing), usize{0});

    // Characters only, over a floor in the default layer: assigned — the view renders characters —
    // and refused per pixel by the receiver's own layer word.
    FrameRun::Options characters = all;
    characters.decals[0].channels = rendering::channel_bit(rendering::NamedChannel::Characters);
    std::vector<u32> elsewhere;
    CY_REQUIRE(render_frame(fixture, characters, elsewhere, &report));
    std::fprintf(stderr, "(b) characters only: %zu pixels differ, %u assignments\n",
                 differing(off, elsewhere), report.decal_assignments);
    CY_CHECK_GT(report.decal_assignments, 0U);
    CY_CHECK_EQ(differing(off, elsewhere), usize{0});

    // Faded out by distance: the camera is five metres away and the fade ends at one.
    FrameRun::Options distant = all;
    distant.decals[0].fade_start = 0.5F;
    distant.decals[0].fade_end = 1.0F;
    std::vector<u32> faded;
    CY_REQUIRE(render_frame(fixture, distant, faded));
    CY_CHECK_EQ(differing(off, faded), usize{0});
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) a decal changes only the pixels inside its projected box") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> off;
    CY_REQUIRE(render_frame(fixture, FrameRun::Options{}, off));

    FrameRun::Options options;
    options.bind = true;
    options.decals = {floor_decal(Vec3{0.6F, kFloorTop, -5.4F}, 0.9F, 0.7F, 0.15F, 0)};
    number(options.decals);
    FrameRun run(fixture, options);
    CY_REQUIRE(run.render());
    const std::vector<u32>& on = run.pixels();
    save("decals-box.png", on);

    const DecalInstance& decal = options.decals[0];
    u32 changed_outside = 0;
    u32 inside = 0;
    u32 inside_unchanged = 0;
    u32 changed = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const usize pixel = (static_cast<usize>(y) * kWidth) + x;
            const Seen what = seen(run.scene(), x, y);
            const bool differs = on[pixel] != off[pixel];
            changed += differs ? 1U : 0U;
            if (what.box < 0) {
                changed_outside += differs ? 1U : 0U;
                continue;
            }
            const f32 margin = inside_margin(decal, what.point);
            if (differs && margin < -kEdgeMargin) {
                ++changed_outside;
            }
            if (margin > kEdgeMargin && interior(run.scene(), x, y, what.box)) {
                ++inside;
                inside_unchanged += differs ? 0U : 1U;
            }
        }
    }
    std::fprintf(stderr,
                 "(c) %u pixels changed; %u changed outside the box; %u well inside, %u of them "
                 "unchanged\n",
                 changed, changed_outside, inside, inside_unchanged);
    CY_CHECK_GT(inside, 500U);
    CY_CHECK_EQ(changed_outside, 0U);
    CY_CHECK_EQ(inside_unchanged, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) a decal is lit: in the receiver's umbra it is what it is with the sun off") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // The receiver's own classification: lit where the frame equals the frame with the shadow map
    // off, umbra where it equals the frame with the sun off.
    FrameRun::Options receiver;
    std::vector<u32> floor_frame;
    std::vector<u32> floor_lit;
    std::vector<u32> floor_dark;
    CY_REQUIRE(render_frame(fixture, receiver, floor_frame));
    receiver.shadowed = false;
    CY_REQUIRE(render_frame(fixture, receiver, floor_lit));
    receiver.shadowed = true;
    receiver.dark = true;
    CY_REQUIRE(render_frame(fixture, receiver, floor_dark));

    // A pale decal across the box's shadow and the lit floor beside it.
    FrameRun::Options options;
    options.bind = true;
    options.decals = {floor_decal(Vec3{-0.2F, kFloorTop, -5.1F}, 1.1F, 0.8F, 0.15F, 0)};
    options.materials[0].albedo = Vec3{0.85F, 0.75F, 0.2F};
    number(options.decals);
    std::vector<u32> decal_frame;
    std::vector<u32> decal_dark;
    CY_REQUIRE(render_frame(fixture, options, decal_frame));
    save("decals-lit.png", decal_frame);
    options.dark = true;
    CY_REQUIRE(render_frame(fixture, options, decal_dark));

    u32 umbra = 0;
    u32 umbra_matching_dark = 0;
    u32 lit = 0;
    u32 lit_matching_dark = 0;
    i64 umbra_brightness = 0;
    i64 lit_brightness = 0;
    for (usize pixel = 0; pixel < decal_frame.size(); ++pixel) {
        if (decal_frame[pixel] == floor_frame[pixel]) {
            continue;  // not decalled
        }
        const bool receiver_lit =
            floor_frame[pixel] == floor_lit[pixel] && floor_frame[pixel] != floor_dark[pixel];
        const bool receiver_umbra =
            floor_frame[pixel] == floor_dark[pixel] && floor_frame[pixel] != floor_lit[pixel];
        if (receiver_umbra) {
            ++umbra;
            umbra_matching_dark += decal_frame[pixel] == decal_dark[pixel] ? 1U : 0U;
            umbra_brightness += brightness(decal_frame[pixel]);
        } else if (receiver_lit) {
            ++lit;
            lit_matching_dark += decal_frame[pixel] == decal_dark[pixel] ? 1U : 0U;
            lit_brightness += brightness(decal_frame[pixel]);
        }
    }
    std::fprintf(stderr,
                 "(d) decalled umbra %u (%u equal to sun-off), decalled lit %u (%u equal to "
                 "sun-off); mean brightness umbra %.1f lit %.1f\n",
                 umbra, umbra_matching_dark, lit, lit_matching_dark,
                 umbra > 0 ? static_cast<double>(umbra_brightness) / umbra : 0.0,
                 lit > 0 ? static_cast<double>(lit_brightness) / lit : 0.0);
    CY_CHECK_GT(umbra, 200U);
    CY_CHECK_GT(lit, 200U);
    // The sun does not reach the decal where it does not reach the floor...
    CY_CHECK_EQ(umbra_matching_dark, umbra);
    // ...and does where it does.
    CY_CHECK_EQ(lit_matching_dark, 0U);
    CY_CHECK_LT(umbra_brightness * lit, lit_brightness * umbra);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(e) a steep receiver fades out rather than taking a stretched decal") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> off;
    CY_REQUIRE(render_frame(fixture, FrameRun::Options{}, off));

    // A downward projector around the resting box's foot: its box holds the floor round the box
    // and the lower half of the box's front face, whose normal is at 90 degrees to the projection.
    FrameRun::Options options;
    options.bind = true;
    options.decals = {
        floor_decal(Vec3{kBoxCentre.x, kFloorTop, kBoxCentre.z + 0.2F}, 1.0F, 1.0F, 0.5F, 0)};
    number(options.decals);
    FrameRun run(fixture, options);
    CY_REQUIRE(run.render());
    const std::vector<u32>& on = run.pixels();
    save("decals-steep.png", on);

    const DecalInstance& decal = options.decals[0];
    u32 face = 0;
    u32 face_changed = 0;
    u32 floor = 0;
    u32 floor_changed = 0;
    for (u32 y = 0; y < kHeight; ++y) {
        for (u32 x = 0; x < kWidth; ++x) {
            const Seen what = seen(run.scene(), x, y);
            if (what.box < 0 || inside_margin(decal, what.point) < kEdgeMargin ||
                !interior(run.scene(), x, y, what.box)) {
                continue;
            }
            const usize pixel = (static_cast<usize>(y) * kWidth) + x;
            const bool differs = on[pixel] != off[pixel];
            if (what.box == 1 && what.normal.z > 0.5F) {
                ++face;
                face_changed += differs ? 1U : 0U;
            } else if (what.box == 0) {
                ++floor;
                floor_changed += differs ? 1U : 0U;
            }
        }
    }
    std::fprintf(stderr, "(e) front face inside the box: %u (%u changed); floor: %u (%u changed)\n",
                 face, face_changed, floor, floor_changed);
    CY_CHECK_GT(face, 100U);
    CY_CHECK_EQ(face_changed, 0U);
    CY_CHECK_GT(floor, 500U);
    CY_CHECK_EQ(floor_changed, floor);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(f) a later decal covers an earlier one, and the order is the decals' own") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // Red and blue, overlapping in the middle.
    DecalInstance red = floor_decal(Vec3{0.1F, kFloorTop, -5.2F}, 0.7F, 0.6F, 0.15F, 0);
    DecalInstance blue = floor_decal(Vec3{0.8F, kFloorTop, -5.2F}, 0.7F, 0.6F, 0.15F, 1);

    const auto frame_of = [&fixture](std::vector<DecalInstance> decals, bool renumber,
                                     std::vector<u32>& out) {
        FrameRun::Options options;
        options.bind = true;
        options.decals = std::move(decals);
        if (renumber) {
            number(options.decals);
        }
        return render_frame(fixture, options, out);
    };
    std::vector<u32> off;
    CY_REQUIRE(render_frame(fixture, FrameRun::Options{}, off));
    std::vector<u32> red_alone;
    std::vector<u32> blue_alone;
    CY_REQUIRE(frame_of({red}, true, red_alone));
    CY_REQUIRE(frame_of({blue}, true, blue_alone));

    red.sort_order = 0;
    blue.sort_order = 1;
    std::vector<u32> blue_over;
    CY_REQUIRE(frame_of({red, blue}, true, blue_over));
    save("decals-order.png", blue_over);
    red.sort_order = 1;
    blue.sort_order = 0;
    std::vector<u32> red_over;
    CY_REQUIRE(frame_of({red, blue}, true, red_over));

    // Where both reach, the later one is the whole answer: each covers fully, so the overlap is
    // the later decal's pixels alone.
    u32 overlap = 0;
    u32 blue_wins = 0;
    u32 red_wins = 0;
    for (usize pixel = 0; pixel < blue_over.size(); ++pixel) {
        const bool both = red_alone[pixel] != off[pixel] && blue_alone[pixel] != off[pixel];
        if (!both) {
            continue;
        }
        ++overlap;
        blue_wins += blue_over[pixel] == blue_alone[pixel] ? 1U : 0U;
        red_wins += red_over[pixel] == red_alone[pixel] ? 1U : 0U;
    }
    std::fprintf(stderr, "(f) %u overlap pixels; blue later covers %u, red later covers %u\n",
                 overlap, blue_wins, red_wins);
    CY_CHECK_GT(overlap, 200U);
    CY_CHECK_EQ(blue_wins, overlap);
    CY_CHECK_EQ(red_wins, overlap);

    // THE ORDER IS THE DECALS' OWN, NOT THE ARRAY'S: the same two decals, same ids and same sort
    // orders, handed over the other way round, draw the same bytes. And equal sort orders break on
    // the id, so a tie is not left to the array either.
    red.id = 1;
    blue.id = 2;
    red.sort_order = 0;
    blue.sort_order = 1;
    std::vector<u32> forwards;
    std::vector<u32> backwards;
    CY_REQUIRE(frame_of({red, blue}, false, forwards));
    CY_REQUIRE(frame_of({blue, red}, false, backwards));
    CY_CHECK_EQ(differing(forwards, backwards), usize{0});
    red.sort_order = 3;
    blue.sort_order = 3;
    CY_REQUIRE(frame_of({red, blue}, false, forwards));
    CY_REQUIRE(frame_of({blue, red}, false, backwards));
    CY_CHECK_EQ(differing(forwards, backwards), usize{0});
    CY_CHECK_EQ(differing(forwards, blue_over), usize{0});  // id 2 is later: blue on top
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(g) the decal lists are clusters, and they lose no decal a pixel needs") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // Several decals of every shape over the floor and the box, including a black splat with
    // relief, so a list that dropped one would show.
    FrameRun::Options options;
    options.bind = true;
    options.decals = {
        floor_decal(Vec3{-2.2F, kFloorTop, -6.5F}, 0.6F, 0.6F, 0.2F, 2),
        floor_decal(Vec3{0.3F, kFloorTop, -5.0F}, 0.5F, 0.4F, 0.2F, 0),
        floor_decal(Vec3{1.9F, kFloorTop, -7.5F}, 0.8F, 0.8F, 0.2F, 1),
        floor_decal(Vec3{kBoxCentre.x, kFloorTop, kBoxCentre.z}, 0.9F, 0.9F, 1.2F, 2),
        floor_decal(Vec3{2.6F, kFloorTop, -4.4F}, 0.4F, 0.4F, 0.2F, 2),
    };
    options.decals[0].weights.normal = 1.0F;
    options.decals[0].weights.roughness = 1.0F;
    options.decals[3].sort_order = 2;
    number(options.decals);

    std::vector<u32> clustered;
    rendering::assembly::AssemblyReport report{};
    CY_REQUIRE(render_frame(fixture, options, clustered, &report));
    save("decals-clustered.png", clustered);
    options.flags = rendering::decals::kDecalListsInTable;
    std::vector<u32> table_lists;
    CY_REQUIRE(render_frame(fixture, options, table_lists));
    options.flags = rendering::decals::kDecalWalkAll;
    std::vector<u32> walked;
    CY_REQUIRE(render_frame(fixture, options, walked));

    std::vector<u32> off;
    CY_REQUIRE(render_frame(fixture, FrameRun::Options{}, off));
    const u32 clusters = report.clusters.clusters;
    std::fprintf(stderr,
                 "(g) %u decals, %u (cluster, decal) assignments over %u clusters; clustered vs "
                 "walked %zu, table lists vs walked %zu, decalled pixels %zu\n",
                 report.decals, report.decal_assignments, clusters, differing(clustered, walked),
                 differing(table_lists, walked), differing(off, walked));
    CY_CHECK_GT(differing(off, walked), usize{2000});
    CY_CHECK_EQ(differing(clustered, walked), usize{0});
    CY_CHECK_EQ(differing(table_lists, walked), usize{0});
    // And the lists really cull: every decal in every cluster would be decals × clusters.
    CY_CHECK_GT(report.decal_assignments, 0U);
    CY_CHECK_LT(report.decal_assignments * 20U, report.decals * clusters);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(h) twenty thousand impact decals draw with no draw call of their own") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    FrameRun::Options none;
    FrameRun empty(fixture, none);
    CY_REQUIRE(empty.render());
    const u32 draws_without = empty.scene().recorded().opaque_draws;

    // A deterministic scatter of 20 000 small scorch marks over the visible floor.
    FrameRun::Options many;
    many.bind = true;
    u32 state = 0x9E3779B9U;
    const auto next = [&state]() {
        state = (state * 1664525U) + 1013904223U;
        return static_cast<f32>(state >> 8U) * (1.0F / 16777216.0F);
    };
    many.decals.reserve(20000);
    for (u32 index = 0; index < 20000U; ++index) {
        const Vec3 centre{-4.0F + (8.0F * next()), kFloorTop, -3.0F - (8.0F * next())};
        DecalInstance decal = floor_decal(centre, 0.06F, 0.06F, 0.05F, 2);
        decal.sort_order = static_cast<i32>(index % 7U);
        many.decals.push_back(decal);
    }
    number(many.decals);
    FrameRun run(fixture, many);
    CY_REQUIRE(run.render());
    save("decals-many.png", run.pixels());
    const u32 draws_with = run.scene().recorded().opaque_draws;
    std::fprintf(stderr,
                 "(h) opaque draws without decals %u, with 20000 decals %u; %u assignments, "
                 "overflow %u; %zu pixels changed\n",
                 draws_without, draws_with, run.report().decal_assignments,
                 run.report().clusters.overflow, differing(empty.pixels(), run.pixels()));
    CY_CHECK_EQ(run.report().decals, 20000U);
    CY_CHECK_EQ(draws_with, draws_without);
    CY_CHECK_GT(run.report().decal_assignments, 10000U);
    CY_CHECK_GT(differing(empty.pixels(), run.pixels()), usize{5000});
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(i) relief tilts the receiver's normal, and zero relief changes nothing") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> off;
    CY_REQUIRE(render_frame(fixture, FrameRun::Options{}, off));

    // Normal only: no albedo, roughness, metallic or emission weight.
    DecalInstance bump = floor_decal(Vec3{0.6F, kFloorTop, -5.2F}, 0.8F, 0.8F, 0.2F, 2);
    bump.weights.albedo = 0.0F;
    bump.weights.normal = 1.0F;
    FrameRun::Options options;
    options.bind = true;
    options.decals = {bump};
    number(options.decals);
    options.materials[2].relief_metres = 0.02F;
    std::vector<u32> tilted;
    CY_REQUIRE(render_frame(fixture, options, tilted));
    save("decals-relief.png", tilted);
    options.materials[2].relief_metres = 0.0F;
    std::vector<u32> flat;
    CY_REQUIRE(render_frame(fixture, options, flat));
    std::fprintf(stderr, "(i) relief changes %zu pixels; zero relief changes %zu\n",
                 differing(off, tilted), differing(off, flat));
    CY_CHECK_GT(differing(off, tilted), usize{100});
    CY_CHECK_EQ(differing(off, flat), usize{0});
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
