// SPDX-License-Identifier: MIT
#pragma once
// The scene the two device suites of this module render: `pipeline_test::FrameScene` with the
// frame's lights replaced, the boxes kept or parked, and each of this module's three parts attached
// on request — the LTC table, the device's light lists and the stochastic path. It reads back the
// frame's HDR colour before the post chain, because a lit pixel is held to the host's shading in
// radiance, and it times the lighting with four timestamps the recorder's extensions write.
//
// Vulkan with validation and synchronisation validation on; a machine with no device skips loudly.

#include "frame_scene.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/lighting/area.h>
#include <cy/rendering/lights_gpu/light_assignment.h>
#include <cy/rendering/lights_gpu/lighting_control.h>
#include <cy/rendering/lights_gpu/ltc_texture.h>
#include <cy/rendering/lights_gpu/many_lights.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace cy::lights_test {

using namespace cy::pipeline_test;
using namespace cy::rendering::lights_gpu;

inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

inline void count_validation(rhi::ValidationSeverity severity, const char* message,
                             void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "graphics validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

class DeviceFixture {
public:
    explicit DeviceFixture(const char* name) noexcept
        : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = name;
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

[[nodiscard]] inline f32 from_half(u16 half) noexcept {
    const u32 sign = (static_cast<u32>(half) & 0x8000U) << 16U;
    const u32 exponent = (static_cast<u32>(half) >> 10U) & 0x1FU;
    const u32 mantissa = static_cast<u32>(half) & 0x3FFU;
    if (exponent == 0) {
        const f32 value = std::ldexp(static_cast<f32>(mantissa), -24);
        return sign != 0 ? -value : value;
    }
    if (exponent == 0x1F) {
        return std::bit_cast<f32>(sign | 0x7F800000U | (mantissa << 13U));
    }
    return std::bit_cast<f32>(sign | ((exponent + 112U) << 23U) | (mantissa << 13U));
}

/// The set 0 slots this suite's textures take, above the scene's own.
inline constexpr u32 kLtcSlot = 110;
inline constexpr u32 kIrradianceSlot = 111;
inline constexpr u32 kReservoirSlot = 112;

/// Where a parked box goes: far under the floor, where no light reaches and no ray sees it.
inline constexpr Vec3 kParked{0.0F, -80.0F, -8.0F};
/// The floor's top: box 0's centre (y = -1.9) plus its half height (0.125).
inline constexpr f32 kFloorTop = -1.775F;

/// The timestamps the recorder's extensions write: the end of the prepare transfer, the end of the
/// depth prepass, the end of the opaque pass and the end of the post-process.
enum Stamp : u32 { kStampPrepare = 0, kStampPrepass, kStampOpaque, kStampPost, kStampCount };

/// One render's configuration and what it produced.
struct LightScene {
    DeviceFixture* fixture = nullptr;
    FrameScene* scene = nullptr;
    /// The frame's lights: the scene's own sun and two points unless `replace_lights`.
    std::vector<render::LightDescription> lights;
    bool replace_lights = true;
    /// Keep the scene's eleven boxes, or park them and leave an open floor.
    bool boxes = true;
    /// The floor slab, box 0: the scene's own 12 m square unless a case widens it.
    Vec3 floor_centre{0.0F, -1.9F, -8.0F};
    Vec3 floor_half{6.0F, 0.125F, 6.0F};
    /// Boxes 1 on, laid out by a case — a room's walls — with their own albedo; every box past the
    /// list is parked. Empty keeps the scene's own boxes (or parks them, without `boxes`).
    struct Box {
        Vec3 centre;
        Vec3 half;
        Vec3 albedo;
    };
    std::vector<Box> layout;
    /// Night: the sun under the horizon, so the sky lights nothing and the frame is its lights.
    bool night = true;
    /// The floor's albedo, as a tint over material 0's colour; zero keeps the material's own.
    Vec3 floor_albedo{0.0F, 0.0F, 0.0F};
    /// The longest list a cluster keeps: the device's bound, above the scene's 32.
    u32 max_lights_per_cluster = 64;
    /// The camera, when a case moves it from the scene's own.
    bool camera = false;
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};
    /// The three parts of this module, each optional.
    const LtcTableTexture* ltc = nullptr;
    GpuLightAssignment* assignment = nullptr;
    ManyLightsPass* many = nullptr;
    /// The view's own exposure for the 8-bit picture, in stops.
    f32 exposure_stops = -11.4F;
    u32 frame_index = 0;

    // Produced.
    std::vector<u16> hdr;  // rgba16f, before the post chain
    rhi::BufferHandle hdr_buffer;
    rhi::QueryPoolHandle timer;
    f64 stamps_ms[kStampCount] = {};
    bool timed = false;
    AssemblyReport report{};
    std::vector<rendering::GpuLight> uploaded;
    /// The view block as uploaded, which the stochastic path reprojects with.
    FrameViewData view{};
    /// The stochastic path's history is cut on the next frame.
    bool cut = false;
};

inline void scene_configure(rendering::assembly::AssemblyDescription& description,
                            void* user) noexcept {
    auto* run = static_cast<LightScene*>(user);
    description.device_light_assignment = run->assignment != nullptr;
    description.many_lights = run->many != nullptr;
    // The jitter pinned, so a pixel's sample position is known to the host and two renders of one
    // scene sample one set of points.
    description.pin_jitter = true;
    description.pinned_jitter_index = 0;
    description.clusters.max_elements_per_cluster = run->max_lights_per_cluster;
}

inline void scene_tint(u32 which, Vec3& tint, void* user) noexcept {
    const auto* run = static_cast<const LightScene*>(user);
    Vec3 albedo = which == 0 ? run->floor_albedo : Vec3{};
    if (which > 0 && which - 1U < run->layout.size()) {
        albedo = run->layout[which - 1U].albedo;
    }
    if (albedo.x <= 0.0F) {
        return;
    }
    const f32* material = kMaterialColours[which % kMaterialCount];
    tint = Vec3{albedo.x / material[0], albedo.y / material[1], albedo.z / material[2]};
}

inline void scene_shape(u32 which, Vec3& centre, Vec3& half, void* user) noexcept {
    const auto* run = static_cast<const LightScene*>(user);
    if (which == 0) {
        centre = run->floor_centre;
        half = run->floor_half;
        return;
    }
    if (run->layout.empty()) {
        return;
    }
    if (which - 1U < run->layout.size()) {
        centre = run->layout[which - 1U].centre;
        half = run->layout[which - 1U].half;
        return;
    }
    centre = kParked;
}

inline void scene_lights(Array<render::LightDescription>& lights, void* user) noexcept {
    auto* run = static_cast<LightScene*>(user);
    if (!run->replace_lights) {
        return;
    }
    lights.clear();
    for (const render::LightDescription& light : run->lights) {
        (void)lights.push_back(light);
    }
}

inline void scene_place(u32 /*which*/, Vec3& centre, f32& /*half*/, void* user) noexcept {
    if (!static_cast<LightScene*>(user)->boxes) {
        centre = kParked;
    }
}

inline void stamp(const ExtensionContext& context, LightScene& run, u32 index) noexcept {
    if (!run.timer) {
        return;
    }
    if (index == kStampPrepare) {
        context.commands->reset_queries(run.timer, 0, kStampCount);
    }
    context.commands->write_timestamp(run.timer, index);
    run.timed = true;
}

inline void stamp_prepare(const ExtensionContext& context, void* user) noexcept {
    stamp(context, *static_cast<LightScene*>(user), kStampPrepare);
}
inline void stamp_prepass(const ExtensionContext& context, void* user) noexcept {
    stamp(context, *static_cast<LightScene*>(user), kStampPrepass);
}
inline void stamp_opaque(const ExtensionContext& context, void* user) noexcept {
    stamp(context, *static_cast<LightScene*>(user), kStampOpaque);
}
inline void stamp_post(const ExtensionContext& context, void* user) noexcept {
    stamp(context, *static_cast<LightScene*>(user), kStampPost);
}

inline Status scene_before_assemble(RenderGraph& graph, rendering::assembly::AssemblyView& view,
                                    FrameSinks& sinks, void* user) noexcept {
    auto* run = static_cast<LightScene*>(user);
    if (run->night) {
        view.sun_direction = Vec3{0.3F, -0.9F, 0.3F};
    }
    const ExtensionRecordFn stamps[kStampCount] = {&stamp_prepare, &stamp_prepass, &stamp_opaque,
                                                   &stamp_post};
    const FramePassKind kinds[kStampCount] = {FramePassKind::Prepare, FramePassKind::DepthPrepass,
                                              FramePassKind::Opaque, FramePassKind::PostProcess};
    for (u32 index = 0; index < kStampCount; ++index) {
        if (Status added = run->scene->recorder().add_extension(
                PassExtension{kinds[index], stamps[index], run});
            !added) {
            return added;
        }
    }
    if (run->assignment != nullptr) {
        const LightListResources lists = run->assignment->import_lists(graph);
        view.cluster_headers = lists.headers;
        view.cluster_indices = lists.indices;
        view.lights_buffer = lists.lights;
        sinks.cluster_assignment = run->assignment->stage();
    }
    if (run->many != nullptr) {
        const ManyLightsTargets targets = run->many->import_targets(graph);
        view.many_lights = targets.irradiance;
        view.many_lights_reservoir = targets.reservoir;
        sinks.many_lights = run->many->stage();
    }
    return ok();
}

inline Status scene_before_upload(FrameUpload& upload, void* user) noexcept {
    auto* run = static_cast<LightScene*>(user);
    FrameScene& scene = *run->scene;
    run->uploaded.assign(upload.lights.begin(), upload.lights.end());
    std::vector<MaterialTextureSlot> slots;
    if (run->ltc != nullptr) {
        slots.push_back(run->ltc->slot(kLtcSlot));
        write_ltc_slot(kLtcSlot, upload.view);
    }
    if (run->assignment != nullptr) {
        const FrameAssembly& assembly = scene.assembly();
        LightAssignmentFrame frame;
        frame.lights = assembly.light_elements();
        frame.host = &assembly.clusters();
        frame.tan_half_fov_y = std::tan(0.9F * 0.5F);
        frame.aspect = static_cast<f32>(scene.width()) / static_cast<f32>(scene.height());
        if (Status set = run->assignment->set_frame(frame); !set) {
            return set;
        }
        upload.cluster_headers_buffer = run->assignment->headers();
        upload.cluster_indices_buffer = run->assignment->indices();
        upload.lights_buffer = run->assignment->lights();
    }
    if (run->many != nullptr) {
        slots.push_back(run->many->irradiance_slot(kIrradianceSlot));
        slots.push_back(run->many->reservoir_slot(kReservoirSlot));
        write_stochastic_lighting(kIrradianceSlot, kReservoirSlot, upload.view);
    }
    upload.globals.exposure_stops = run->exposure_stops;
    run->view = upload.view;
    if (!slots.empty()) {
        if (Status set = scene.set_frame_textures({slots.data(), slots.size()}); !set) {
            return set;
        }
    }
    return ok();
}

struct HdrReadback {
    ResourceId colour = kInvalidResource;
    rhi::BufferHandle buffer;
    u32 width = 0;
    u32 height = 0;
};

inline void record_hdr(const PassContext& context, void* user) noexcept {
    auto* readback = static_cast<HdrReadback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{readback->width, readback->height, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(readback->colour),
                                             readback->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

inline HdrReadback& hdr_readback() noexcept {
    static HdrReadback readback;
    return readback;
}

inline Status scene_after_assemble(RenderGraph& graph, const FrameResources& resources,
                                   void* user) noexcept {
    auto* run = static_cast<LightScene*>(user);
    if (run->many != nullptr) {
        // After the upload: the light buffer the forward pass reads this frame, and the view block
        // as it was uploaded.
        ManyLightsFrame frame;
        frame.lights = {run->uploaded.data(), run->uploaded.size()};
        frame.light_buffer = run->scene->bindings().staged_lights();
        frame.view = &run->view;
        frame.frame_index = run->frame_index;
        frame.history_invalid = run->cut;
        run->cut = false;
        if (Status set = run->many->set_frame(frame); !set) {
            return set;
        }
    }
    if (run->hdr_buffer.is_null()) {
        return ok();
    }
    HdrReadback& readback = hdr_readback();
    readback.colour = resources.color;
    readback.buffer = run->hdr_buffer;
    readback.width = run->scene->width();
    readback.height = run->scene->height();
    BufferRequest request;
    request.name = "lights test hdr";
    request.size = u64{readback.width} * readback.height * 8U;
    request.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId destination = graph.import_buffer(request, run->hdr_buffer);
    graph.add_pass("lights test hdr", rhi::QueueKind::Graphics)
        .read(resources.color, rhi::Access::TransferRead)
        .write(destination, rhi::Access::TransferWrite)
        .record(&record_hdr, &readback);
    graph.add_pass("lights test hdr host", rhi::QueueKind::Graphics)
        .read(destination, rhi::Access::HostRead)
        .side_effect();
    return ok();
}

/// Build `scene` for `run`: its hooks, its extent, its buffers.
[[nodiscard]] inline bool build_scene(LightScene& run, FrameScene& scene, u32 width, u32 height,
                                      u32 light_capacity) {
    run.scene = &scene;
    FrameSceneHooks hooks;
    hooks.user = &run;
    hooks.configure = &scene_configure;
    hooks.lights = &scene_lights;
    hooks.place_box = &scene_place;
    hooks.shape_box = &scene_shape;
    hooks.tint_box = &scene_tint;
    hooks.before_assemble = &scene_before_assemble;
    hooks.before_upload = &scene_before_upload;
    hooks.after_assemble = &scene_after_assemble;
    scene.set_hooks(hooks);
    scene.set_extent(width, height);
    scene.set_light_capacity(light_capacity);
    scene.set_read_back(true);
    rhi::Device& device = run.fixture->device();
    if (Status built = scene.build(device); !built) {
        std::fprintf(stderr, "lights test: the scene did not build: %s\n", built.error().message);
        return false;
    }
    if (run.camera) {
        scene.set_camera(run.eye, normalize(run.forward));
    }
    rhi::BufferDescription hdr;
    hdr.name = "lights test hdr";
    hdr.size = u64{width} * height * 8U;
    hdr.usage = rhi::BufferUsage::TransferDestination;
    hdr.memory = rhi::MemoryUse::Readback;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(hdr);
    if (!buffer.has_value()) {
        return false;
    }
    run.hdr_buffer = *buffer;
    if (device.capabilities().has(rhi::Capability::TimestampQueries)) {
        rhi::QueryPoolDescription timer;
        timer.name = "lights test timestamps";
        timer.kind = rhi::QueryKind::Timestamp;
        timer.count = kStampCount;
        if (Expected<rhi::QueryPoolHandle, Error> pool = device.create_query_pool(timer); pool) {
            run.timer = *pool;
        }
    }
    return true;
}

inline void release_scene(LightScene& run) noexcept {
    rhi::Device& device = run.fixture->device();
    (void)device.wait_idle();
    if (!run.hdr_buffer.is_null()) {
        device.destroy_buffer(run.hdr_buffer);
    }
    run.hdr_buffer = {};
    if (run.timer) {
        device.destroy_query_pool(run.timer);
    }
    run.timer = {};
}

/// Render one frame; the HDR colour into `run.hdr`, the stamps into `run.stamps_ms`.
[[nodiscard]] inline bool render_frame(LightScene& run) {
    run.timed = false;
    // ONE SUB-PIXEL OFFSET FOR EVERY FRAME. `JitterSequence::pin` fixes where the sequence starts
    // and the frames after it step on, so pinning it again before each frame holds every frame at
    // one sample: a pixel's surface point is then the same point every frame, and the host's sum
    // at it is the sum the frames' estimates average to.
    run.scene->assembly().temporal().pin_jitter(0);
    if (Status rendered = run.scene->render(RecordMode::Callbacks, run.report); !rendered) {
        std::fprintf(stderr, "lights test: the frame did not render: %s\n",
                     rendered.error().message);
        return false;
    }
    rhi::Device& device = run.fixture->device();
    const u32 width = run.scene->width();
    const u32 height = run.scene->height();
    const auto* mapped = static_cast<const u16*>(device.buffer_mapped_pointer(run.hdr_buffer));
    if (mapped == nullptr) {
        return false;
    }
    run.hdr.assign(mapped, mapped + (static_cast<usize>(width) * height * 4U));
    if (run.timed && run.timer) {
        u64 stamps[kStampCount] = {};
        Expected<u32, Error> read =
            device.read_query_results(run.timer, 0, kStampCount, Span<u64>(stamps, kStampCount));
        if (read && *read == kStampCount) {
            for (u32 index = 0; index < kStampCount; ++index) {
                run.stamps_ms[index] = static_cast<f64>(stamps[index] - stamps[0]) / 1.0e6;
            }
        }
    }
    ++run.frame_index;
    return true;
}

/// The HDR colour at a pixel, as floats.
[[nodiscard]] inline Vec3 hdr_at(const LightScene& run, u32 x, u32 y) noexcept {
    const usize at = ((static_cast<usize>(y) * run.scene->width()) + x) * 4U;
    return Vec3{from_half(run.hdr[at]), from_half(run.hdr[at + 1]), from_half(run.hdr[at + 2])};
}

/// The floor point a pixel's centre sees, camera-relative, through the scene's projection and the
/// frame's jitter; false when the ray misses the floor's top.
[[nodiscard]] inline bool floor_point(const LightScene& run, u32 x, u32 y,
                                      Vec3& relative) noexcept {
    const FrameScene& scene = *run.scene;
    const f32 width = static_cast<f32>(scene.width());
    const f32 height = static_cast<f32>(scene.height());
    // The jitter moves the projection by `jitter * 2 / extent` in NDC, so a pixel centre's
    // unjittered NDC is its own less that.
    const f32 ndc_x = (((static_cast<f32>(x) + 0.5F) / width) * 2.0F) - 1.0F -
                      (run.report.jitter.x * 2.0F / width);
    const f32 ndc_y = 1.0F - (((static_cast<f32>(y) + 0.5F) / height) * 2.0F) -
                      (run.report.jitter.y * 2.0F / height);
    const f32 tan_y = std::tan(0.45F);
    const f32 tan_x = tan_y * (width / height);
    const Vec3 view_direction{ndc_x * tan_x, ndc_y * tan_y, -1.0F};
    // The scene's camera looks down -Z from its eye with no rotation unless a suite turned it.
    const Mat4& rotation = scene.relative_view();
    const Vec3 direction = normalize(Vec3{
        (rotation.columns[0].x * view_direction.x) + (rotation.columns[0].y * view_direction.y) +
            (rotation.columns[0].z * view_direction.z),
        (rotation.columns[1].x * view_direction.x) + (rotation.columns[1].y * view_direction.y) +
            (rotation.columns[1].z * view_direction.z),
        (rotation.columns[2].x * view_direction.x) + (rotation.columns[2].y * view_direction.y) +
            (rotation.columns[2].z * view_direction.z)});
    const Vec3 eye = scene.eye();
    if (direction.y >= -1e-4F) {
        return false;
    }
    const f32 top = run.floor_centre.y + run.floor_half.y;
    const f32 t = (top - eye.y) / direction.y;
    relative = direction * t;
    const Vec3 world = eye + relative;
    const Vec3 inside = run.floor_half - Vec3{0.1F, 0.0F, 0.1F};
    return std::fabs(world.x - run.floor_centre.x) < inside.x &&
           std::fabs(world.z - run.floor_centre.z) < inside.z;
}

}  // namespace cy::lights_test
