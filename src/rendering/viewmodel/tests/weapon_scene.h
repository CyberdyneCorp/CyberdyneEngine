// SPDX-License-Identifier: MIT
#pragma once
// The scene `render.weapon_view` renders: `pipeline_test::FrameScene` with a first-person weapon —
// five boxes on a layer of their own, a receiver, a barrel, a grip, a magazine and a sleeve, held
// in the camera's space and turned with it — a wall the barrel can be pushed into, the world's
// field of view and the weapon's set per case, and the frame read back before the post chain: the
// HDR colour and the prepass motion vectors.
//
// THE WEAPON'S PIXELS ARE FOUND BY DIFFERENCE. A frame with the weapon and the same frame with the
// weapon parked under the floor differ exactly where the weapon is drawn: it casts nothing unless a
// case asks, nothing else in the scene reads it, and every other draw is the same draw. So a mask
// is a measurement of what was drawn, not a prediction of where the boxes should land.
//
// Vulkan with validation and synchronisation validation on; a machine with no device skips loudly.

#include "frame_scene.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/viewmodel/viewmodel.h>

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace cy::weapon_test {

using namespace cy::pipeline_test;
using namespace cy::rendering::viewmodel;

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

/// The layer the arms and the weapon are on.
inline constexpr render::LayerMask kWeaponLayer = 1U << 5U;
/// The scene's boxes: the floor is 0, the wall 1, the weapon's five parts 2 to 6, and 7 on are the
/// scene's own ring of boxes — kept for a picture, parked for a measurement.
inline constexpr u32 kWall = 1;
inline constexpr u32 kFirstPart = 2;
inline constexpr u32 kPartCount = 5;
inline constexpr u32 kFirstWorld = kFirstPart + kPartCount;
/// Where a parked box goes: far under the floor, out of every view.
inline constexpr Vec3 kParked{0.0F, -90.0F, 0.0F};

/// The set 0 slot the shadow map takes.
inline constexpr u32 kShadowSlot = 121;
inline constexpr u32 kShadowExtent = 2048;
inline constexpr f32 kShadowRadius = 5.0F;
inline const Vec3 kShadowCentre{0.0F, -1.0F, -2.0F};

/// One part of the weapon, in the WEAPON's space (−Z forward along the barrel), metres.
struct Part {
    Vec3 centre;
    Vec3 half;
    Vec3 albedo;
};

/// The weapon: a receiver, a barrel, a grip, a magazine and the sleeve of the arm holding it. Its
/// origin is the grip's top; held at the hip it reaches from 0.11 m to 0.85 m in front of the eye —
/// past the world's 0.1 m near plane, so the same boxes drawn as world geometry are whole too.
inline constexpr Part kParts[kPartCount] = {
    {{0.0F, 0.0F, -0.12F}, {0.026F, 0.034F, 0.13F}, {0.30F, 0.31F, 0.33F}},
    {{0.0F, 0.012F, -0.40F}, {0.011F, 0.011F, 0.15F}, {0.08F, 0.08F, 0.09F}},
    {{0.0F, -0.07F, 0.0F}, {0.017F, 0.045F, 0.022F}, {0.32F, 0.20F, 0.11F}},
    {{0.0F, -0.08F, -0.15F}, {0.014F, 0.052F, 0.026F}, {0.24F, 0.16F, 0.09F}},
    {{0.035F, -0.13F, 0.08F}, {0.045F, 0.045F, 0.11F}, {0.22F, 0.27F, 0.16F}},
};

/// The sun's direction of travel: low, from behind the camera's left shoulder, so a weapon held at
/// the hip casts onto floor the camera sees.
[[nodiscard]] inline Vec3 sun_behind() noexcept {
    const Vec3 travel{0.25F, -0.55F, -0.80F};
    return travel * (1.0F / std::sqrt(dot(travel, travel)));
}

/// Low and ahead of the camera, so a slab the camera sees shades the weapon held under it. A shadow
/// caster is a draw of the frame, so an occluder out of view casts nothing.
[[nodiscard]] inline Vec3 sun_ahead() noexcept {
    const Vec3 travel{0.25F, -0.55F, 0.80F};
    return travel * (1.0F / std::sqrt(dot(travel, travel)));
}

/// One render's configuration and what it produced.
struct WeaponRun {
    DeviceFixture* fixture = nullptr;
    FrameScene* scene = nullptr;

    // --- The configuration, read every frame -----------------------------------------------
    /// Draw the weapon through the weapon view. Off, its boxes are ordinary world geometry on a
    /// layer nothing excludes: the "without the separate view" picture.
    bool weapon_view = true;
    ViewmodelSettings settings = [] {
        ViewmodelSettings made;
        made.layers = kWeaponLayer;
        return made;
    }();
    /// Draw the weapon at all, or park it: what a mask is the difference of.
    bool weapon_present = true;
    /// Where the weapon's origin is held in the EYE's space, and how it is turned there.
    Vec3 hold{0.16F, -0.15F, -0.30F};
    Quat grip = Quat::identity();
    /// The wall a pushed weapon goes into: its front face's distance ahead of the eye, or none.
    bool wall = true;
    f32 wall_distance = 0.45F;
    /// The wall's centre across the view and its half width: a whole wall by default, a pillar
    /// the barrel reaches into for a picture with the world behind it.
    f32 wall_x = 0.0F;
    f32 wall_half_width = 2.5F;
    Vec3 wall_albedo{0.62F, 0.58F, 0.52F};
    /// Keep the scene's own ring of boxes behind, for a picture.
    bool ring = false;
    /// A small light beside the weapon — a lantern on the sleeve — whose reach is a few
    /// centimetres more than the weapon is wide: the light a wrong cluster list would miss.
    bool lantern = false;
    /// The sun's shadow map, and what the weapon does in it.
    bool shadows = false;
    /// A box over the weapon that shadows it from the sun.
    bool overhang = false;
    /// The sun's direction of travel, read every frame.
    Vec3 travel = sun_behind();
    f32 exposure_stops = -12.2F;

    // --- Produced ---------------------------------------------------------------------------
    std::vector<u16> hdr;       // rgba16f, before the post chain
    std::vector<u16> velocity;  // rg16f, the prepass motion vectors
    rhi::BufferHandle hdr_buffer;
    rhi::BufferHandle velocity_buffer;
    rhi::TextureHandle shadow_color;
    rhi::TextureHandle shadow_depth;
    rhi::TextureViewHandle shadow_view;
    rhi::QueryPoolHandle timer;
    f64 stamps_ms[4] = {};
    bool timed = false;
    AssemblyReport report{};
    ViewmodelFrame lens;
    ViewmodelMatrices matrices;
    FrameViewData view{};
    std::vector<rendering::GpuLight> lights;
    rendering::FrameResourceRead reads[1] = {};
};

[[nodiscard]] inline Mat4 shadow_to_clip(Vec3 travel) noexcept {
    const Vec3 eye = kShadowCentre - (travel * (kShadowRadius * 2.0F));
    const Mat4 view = look_at(eye, kShadowCentre, Vec3{0.0F, 1.0F, 0.0F});
    const Mat4 projection = orthographic_reversed_z(-kShadowRadius, kShadowRadius, -kShadowRadius,
                                                    kShadowRadius, 0.1F, kShadowRadius * 4.0F);
    return projection * view;
}

/// The camera's rotation in the world: the transpose of the scene's camera-relative view.
[[nodiscard]] inline Mat3 eye_rotation(const FrameScene& scene) noexcept {
    const Mat4& view = scene.relative_view();
    Mat3 rotation;
    for (u32 row = 0; row < 3; ++row) {
        for (u32 column = 0; column < 3; ++column) {
            rotation.columns[column][row] = view.columns[row][column];
        }
    }
    return rotation;
}

[[nodiscard]] inline Vec3 apply(const Mat3& m, Vec3 v) noexcept {
    return (m.columns[0] * v.x) + (m.columns[1] * v.y) + (m.columns[2] * v.z);
}

inline void scene_configure(AssemblyDescription& description, void* /*user*/) noexcept {
    // The jitter pinned, so two renders of one scene sample one set of points.
    description.pin_jitter = true;
    description.pinned_jitter_index = 0;
}

inline void scene_pipelines(PipelineSetup& setup, void* /*user*/) noexcept {
    setup.viewmodel = true;
}

inline render::LayerMask scene_layer(u32 which, void* /*user*/) noexcept {
    return which >= kFirstPart && which < kFirstWorld ? kWeaponLayer : render::kDefaultLayer;
}

inline void scene_tint(u32 which, Vec3& tint, void* user) noexcept {
    const auto* run = static_cast<const WeaponRun*>(user);
    Vec3 albedo{0.0F, 0.0F, 0.0F};
    if (which == kWall) {
        albedo = run->wall_albedo;
    } else if (which >= kFirstPart && which < kFirstWorld) {
        albedo = kParts[which - kFirstPart].albedo;
    } else {
        return;
    }
    const f32* material = kMaterialColours[which % kMaterialCount];
    tint = Vec3{albedo.x / material[0], albedo.y / material[1], albedo.z / material[2]};
}

inline void scene_shape(u32 which, Vec3& centre, Vec3& half, void* user) noexcept {
    const auto* run = static_cast<const WeaponRun*>(user);
    if (which == kWall) {
        // Built for the default eye; a case that moves the camera keeps the wall where it was.
        centre = Vec3{run->wall_x, 0.0F, -(run->wall_distance + 0.1F)};
        half = Vec3{run->wall_half_width, 2.0F, 0.1F};
        return;
    }
    if (which >= kFirstPart && which < kFirstWorld) {
        // THE CULL'S BOUND, NOT THE BOX: a generous box around the eye, so the weapon is never
        // culled for being turned or raised. The row the scene draws is written by `scene_row`.
        centre = Vec3{0.0F, 0.0F, 0.0F};
        half = Vec3{1.2F, 1.2F, 1.2F};
        return;
    }
    if (which == kFirstWorld && run->overhang) {
        // A slab ahead of and over the weapon, between it and a sun ahead: in the camera's view,
        // as a shadow caster must be.
        centre = Vec3{-0.2F, 0.6F, -1.5F};
        half = Vec3{0.8F, 0.05F, 0.8F};
        return;
    }
    if (which >= kFirstWorld && !run->ring) {
        centre = kParked;
    }
}

/// A weapon part's row: the part in the weapon's space, the weapon in the eye's, the eye in the
/// world — camera-relative, as every row is.
inline void scene_row(u32 which, InstanceTransform& row, void* user) noexcept {
    const auto* run = static_cast<const WeaponRun*>(user);
    if (which < kFirstPart || which >= kFirstWorld || run->scene == nullptr) {
        return;
    }
    const Part& part = kParts[which - kFirstPart];
    const Mat3 eye = eye_rotation(*run->scene);
    const Mat3 grip = Mat3::from_quat(run->grip);
    // The part's three axes in camera-relative space: the grip's columns, turned by the eye.
    const Vec3 axes[3] = {apply(eye, grip.columns[0]), apply(eye, grip.columns[1]),
                          apply(eye, grip.columns[2])};
    const f32 sizes[3] = {part.half.x * 2.0F, part.half.y * 2.0F, part.half.z * 2.0F};
    Vec3 at = apply(eye, run->hold + apply(grip, part.centre));
    if (!run->weapon_present) {
        at = kParked;
    }
    f32* rows[3] = {row.row0, row.row1, row.row2};
    for (u32 r = 0; r < 3U; ++r) {
        for (u32 c = 0; c < 3U; ++c) {
            rows[r][c] = axes[c][r] * sizes[c];
        }
    }
    row.row0[3] = at.x;
    row.row1[3] = at.y;
    row.row2[3] = at.z;
}

inline void scene_lights(Array<render::LightDescription>& lights, void* user) noexcept {
    const auto* run = static_cast<const WeaponRun*>(user);
    lights.clear();
    render::LightDescription sun;
    sun.kind = render::LightKind::Directional;
    sun.intensity = 22000.0F;
    sun.transform.rotation = Quat::look_rotation(run->travel, Vec3{0.0F, 1.0F, 0.0F});
    sun.color[0] = 1.0F;
    sun.color[1] = 0.96F;
    sun.color[2] = 0.9F;
    sun.stable_id = 1;
    sun.casts_shadow = true;
    (void)lights.push_back(sun);
    if (run->lantern) {
        // Beside the sleeve, right of the receiver, reaching the weapon's right flank and nothing
        // a few centimetres past it.
        render::LightDescription lantern;
        lantern.kind = render::LightKind::Point;
        lantern.intensity = 260.0F;
        lantern.range = 0.16F;
        lantern.transform = Transform::from_translation(run->hold + Vec3{0.075F, -0.02F, -0.08F});
        lantern.color[0] = 1.0F;
        lantern.color[1] = 0.62F;
        lantern.color[2] = 0.30F;
        lantern.stable_id = 2;
        (void)lights.push_back(lantern);
    }
}

inline Status scene_before_assemble(RenderGraph& graph, rendering::assembly::AssemblyView& view,
                                    FrameSinks& sinks, void* user) noexcept {
    auto* run = static_cast<WeaponRun*>(user);
    FrameScene& scene = *run->scene;
    if (run->weapon_view) {
        view.viewmodel =
            assembly_view(run->settings, view.view,
                          static_cast<f32>(scene.width()) / static_cast<f32>(scene.height()));
    }
    if (run->timer) {
        // The four stamps: the end of the prepare transfer, the prepass, the opaque pass and the
        // post-process, through the recorder's own extension seam.
        const FramePassKind kinds[4] = {FramePassKind::Prepare, FramePassKind::DepthPrepass,
                                        FramePassKind::Opaque, FramePassKind::PostProcess};
        for (u32 index = 0; index < 4U; ++index) {
            const ExtensionRecordFn stamp = [](const ExtensionContext& context,
                                               void* data) noexcept {
                auto* timed = static_cast<WeaponRun*>(data);
                const u32 at = context.kind == FramePassKind::Prepare        ? 0U
                               : context.kind == FramePassKind::DepthPrepass ? 1U
                               : context.kind == FramePassKind::Opaque       ? 2U
                                                                             : 3U;
                if (at == 0U) {
                    context.commands->reset_queries(timed->timer, 0, 4);
                }
                context.commands->write_timestamp(timed->timer, at);
                timed->timed = true;
            };
            if (Status added =
                    scene.recorder().add_extension(PassExtension{kinds[index], stamp, run});
                !added) {
                return added;
            }
        }
    }
    if (!run->shadows) {
        return ok();
    }
    TextureRequest request;
    request.name = "weapon test shadow map";
    request.format = rhi::Format::R32Sfloat;
    request.width = kShadowExtent;
    request.height = kShadowExtent;
    view.shadow_color = graph.import_texture(request, run->shadow_color, rhi::ImageUse::Undefined);
    request.name = "weapon test shadow depth";
    request.format = rhi::Format::D32Sfloat;
    view.shadow_depth = graph.import_texture(request, run->shadow_depth, rhi::ImageUse::Undefined);
    FrameRecorder& recorder = scene.recorder();
    recorder.set_shadow_targets(view.shadow_color, view.shadow_depth, kShadowExtent);
    const auto shadow = static_cast<usize>(FramePassKind::Shadow);
    sinks.passes[shadow] = recorder.sinks().passes[shadow];
    const auto opaque = static_cast<usize>(FramePassKind::Opaque);
    run->reads[0] =
        rendering::FrameResourceRead{view.shadow_color, rhi::Access::FragmentSampledRead};
    sinks.passes[opaque].reads = Span<const rendering::FrameResourceRead>(run->reads, 1);
    return ok();
}

inline Status scene_before_upload(FrameUpload& upload, void* user) noexcept {
    auto* run = static_cast<WeaponRun*>(user);
    FrameScene& scene = *run->scene;
    FrameAssembly& assembly = scene.assembly();
    upload.globals.exposure_stops = run->exposure_stops;
    if (run->weapon_view) {
        const f32 aspect = static_cast<f32>(scene.width()) / static_cast<f32>(scene.height());
        run->matrices =
            run->lens.advance(run->settings, aspect, scene.relative_view(), assembly.temporal());
        write_viewmodel(run->matrices, run->settings, assembly.temporal().jitter().current(),
                        assembly.render_width(), assembly.render_height(), upload.view);
    }
    if (run->shadows) {
        run->lights.assign(upload.lights.begin(), upload.lights.end());
        u32 sun = 0;
        for (u32 index = 0; index < run->lights.size(); ++index) {
            if (run->lights[index].kind == rendering::kGpuLightDirectional) {
                sun = index;
                const Vec3 travel = run->travel;
                run->lights[index].direction[0] = travel.x;
                run->lights[index].direction[1] = travel.y;
                run->lights[index].direction[2] = travel.z;
                break;
            }
        }
        upload.lights = Span<const rendering::GpuLight>(run->lights.data(), run->lights.size());
        const Mat4 to_clip = shadow_to_clip(run->travel);
        for (u32 row = 0; row < 4; ++row) {
            for (u32 column = 0; column < 4; ++column) {
                upload.view.shadow_to_clip[(row * 4U) + column] = to_clip.at(row, column);
            }
        }
        upload.view.shadow_control[0] = kShadowSlot;
        upload.view.shadow_control[1] = sun;
        upload.view.shadow_control[2] = kShadowExtent;
        upload.view.shadow_control[3] = 1U;
        const MaterialTextureSlot slots[1] = {{kShadowSlot, run->shadow_view}};
        if (Status set = scene.set_frame_textures(Span<const MaterialTextureSlot>(slots, 1));
            !set) {
            return set;
        }
    }
    run->view = upload.view;
    return ok();
}

struct Readback {
    ResourceId source = kInvalidResource;
    rhi::BufferHandle buffer;
    u32 width = 0;
    u32 height = 0;
};

inline void record_readback(const PassContext& context, void* user) noexcept {
    auto* readback = static_cast<Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{readback->width, readback->height, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(readback->source),
                                             readback->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

inline Readback (&readbacks() noexcept)[2] {
    static Readback storage[2];
    return storage;
}

inline void declare_readback(RenderGraph& graph, Readback& readback, const char* name,
                             u64 bytes) noexcept {
    BufferRequest request;
    request.name = name;
    request.size = bytes;
    request.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId destination = graph.import_buffer(request, readback.buffer);
    graph.add_pass(name, rhi::QueueKind::Graphics)
        .read(readback.source, rhi::Access::TransferRead)
        .write(destination, rhi::Access::TransferWrite)
        .record(&record_readback, &readback);
    graph.add_pass(name, rhi::QueueKind::Graphics)
        .read(destination, rhi::Access::HostRead)
        .side_effect();
}

inline Status scene_after_assemble(RenderGraph& graph, const FrameResources& resources,
                                   void* user) noexcept {
    auto* run = static_cast<WeaponRun*>(user);
    const u32 width = run->scene->width();
    const u32 height = run->scene->height();
    Readback(&slots)[2] = readbacks();
    slots[0] = Readback{resources.color, run->hdr_buffer, width, height};
    declare_readback(graph, slots[0], "weapon test hdr", u64{width} * height * 8U);
    if (resources.velocity != kInvalidResource) {
        slots[1] = Readback{resources.velocity, run->velocity_buffer, width, height};
        declare_readback(graph, slots[1], "weapon test velocity", u64{width} * height * 4U);
    }
    return ok();
}

[[nodiscard]] inline bool make_buffer(rhi::Device& device, const char* name, u64 bytes,
                                      rhi::BufferHandle& out) {
    rhi::BufferDescription description;
    description.name = name;
    description.size = bytes;
    description.usage = rhi::BufferUsage::TransferDestination;
    description.memory = rhi::MemoryUse::Readback;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(description);
    if (!buffer.has_value()) {
        return false;
    }
    out = *buffer;
    return true;
}

[[nodiscard]] inline bool make_shadow_map(WeaponRun& run) {
    rhi::Device& device = run.fixture->device();
    rhi::TextureDescription texture;
    texture.name = "weapon test shadow map";
    texture.format = rhi::Format::R32Sfloat;
    texture.extent = rhi::Extent3D{kShadowExtent, kShadowExtent, 1};
    texture.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
    auto color = device.create_texture(texture);
    if (!color.has_value()) {
        return false;
    }
    run.shadow_color = *color;
    texture.name = "weapon test shadow depth";
    texture.format = rhi::Format::D32Sfloat;
    texture.usage = rhi::TextureUsage::DepthStencilAttachment;
    auto depth = device.create_texture(texture);
    if (!depth.has_value()) {
        return false;
    }
    run.shadow_depth = *depth;
    rhi::TextureViewDescription view;
    view.name = "weapon test shadow map";
    view.texture = run.shadow_color;
    auto made = device.create_texture_view(view);
    if (!made.has_value()) {
        return false;
    }
    run.shadow_view = *made;
    return true;
}

/// Build `scene` for `run`: its hooks, its extent, its buffers.
[[nodiscard]] inline bool build_scene(WeaponRun& run, FrameScene& scene, u32 width, u32 height) {
    run.scene = &scene;
    FrameSceneHooks hooks;
    hooks.user = &run;
    hooks.configure = &scene_configure;
    hooks.pipelines = &scene_pipelines;
    hooks.layer_mask = &scene_layer;
    hooks.tint_box = &scene_tint;
    hooks.shape_box = &scene_shape;
    hooks.place_row = &scene_row;
    hooks.lights = &scene_lights;
    hooks.before_assemble = &scene_before_assemble;
    hooks.before_upload = &scene_before_upload;
    hooks.after_assemble = &scene_after_assemble;
    scene.set_hooks(hooks);
    scene.set_extent(width, height);
    scene.set_read_back(true);
    rhi::Device& device = run.fixture->device();
    if (Status built = scene.build(device); !built) {
        std::fprintf(stderr, "weapon test: the scene did not build: %s\n", built.error().message);
        return false;
    }
    if (!make_buffer(device, "weapon test hdr", u64{width} * height * 8U, run.hdr_buffer) ||
        !make_buffer(device, "weapon test velocity", u64{width} * height * 4U,
                     run.velocity_buffer) ||
        !make_shadow_map(run)) {
        return false;
    }
    if (device.capabilities().has(rhi::Capability::TimestampQueries)) {
        rhi::QueryPoolDescription timer;
        timer.name = "weapon test timestamps";
        timer.kind = rhi::QueryKind::Timestamp;
        timer.count = 4;
        if (Expected<rhi::QueryPoolHandle, Error> pool = device.create_query_pool(timer); pool) {
            run.timer = *pool;
        }
    }
    // The rows are written from the camera, so the weapon's are placed once the scene has one.
    scene.set_camera(scene.eye(), scene.forward());
    return true;
}

inline void release_scene(WeaponRun& run) noexcept {
    rhi::Device& device = run.fixture->device();
    (void)device.wait_idle();
    for (rhi::BufferHandle* buffer : {&run.hdr_buffer, &run.velocity_buffer}) {
        if (!buffer->is_null()) {
            device.destroy_buffer(*buffer);
        }
        *buffer = {};
    }
    if (!run.shadow_view.is_null()) {
        device.destroy_texture_view(run.shadow_view);
    }
    for (rhi::TextureHandle* texture : {&run.shadow_color, &run.shadow_depth}) {
        if (!texture->is_null()) {
            device.destroy_texture(*texture);
        }
        *texture = {};
    }
    if (run.timer) {
        device.destroy_query_pool(run.timer);
    }
    run.timer = {};
}

/// Render one frame: the HDR colour into `run.hdr`, the motion into `run.velocity`. The rows are
/// placed again first, so a change to the weapon's hold reaches the frame.
[[nodiscard]] inline bool render_frame(WeaponRun& run) {
    FrameScene& scene = *run.scene;
    scene.set_camera(scene.eye(), scene.forward());
    // ONE SUB-PIXEL OFFSET FOR EVERY FRAME: `pin` fixes where the sequence starts and the frames
    // after it step on, so pinning again before each frame holds every frame at one sample — two
    // frames of one scene then differ only where the scene does.
    scene.assembly().temporal().pin_jitter(0);
    run.timed = false;
    if (Status rendered = scene.render(RecordMode::Callbacks, run.report); !rendered) {
        std::fprintf(stderr, "weapon test: the frame did not render: %s\n",
                     rendered.error().message);
        return false;
    }
    rhi::Device& device = run.fixture->device();
    const usize texels = static_cast<usize>(scene.width()) * scene.height();
    const auto* hdr = static_cast<const u16*>(device.buffer_mapped_pointer(run.hdr_buffer));
    const auto* motion = static_cast<const u16*>(device.buffer_mapped_pointer(run.velocity_buffer));
    if (hdr == nullptr || motion == nullptr) {
        return false;
    }
    run.hdr.assign(hdr, hdr + (texels * 4U));
    run.velocity.assign(motion, motion + (texels * 2U));
    if (run.timed && run.timer) {
        u64 stamps[4] = {};
        Expected<u32, Error> read =
            device.read_query_results(run.timer, 0, 4, Span<u64>(stamps, 4));
        if (read && *read == 4U) {
            for (u32 index = 0; index < 4U; ++index) {
                run.stamps_ms[index] = static_cast<f64>(stamps[index] - stamps[0]) / 1.0e6;
            }
        }
    }
    return true;
}

/// The texels the weapon covers: where a frame with it and the same frame without it differ.
/// Renders the weapon-less frame itself and leaves `run` with the weapon's frame.
[[nodiscard]] inline bool weapon_mask(WeaponRun& run, std::vector<u8>& mask) {
    run.weapon_present = false;
    run.lens.reset();
    if (!render_frame(run)) {
        return false;
    }
    const std::vector<u16> empty = run.hdr;
    run.weapon_present = true;
    run.lens.reset();
    if (!render_frame(run)) {
        return false;
    }
    const usize texels = run.hdr.size() / 4U;
    mask.assign(texels, 0);
    for (usize texel = 0; texel < texels; ++texel) {
        for (u32 channel = 0; channel < 3U; ++channel) {
            if (run.hdr[(texel * 4U) + channel] != empty[(texel * 4U) + channel]) {
                mask[texel] = 1;
                break;
            }
        }
    }
    return true;
}

[[nodiscard]] inline u32 count(const std::vector<u8>& mask) noexcept {
    u32 total = 0;
    for (const u8 texel : mask) {
        total += texel;
    }
    return total;
}

/// The HDR colour at a texel.
[[nodiscard]] inline Vec3 hdr_at(const WeaponRun& run, usize texel) noexcept {
    return Vec3{from_half(run.hdr[(texel * 4U) + 0]), from_half(run.hdr[(texel * 4U) + 1]),
                from_half(run.hdr[(texel * 4U) + 2])};
}

[[nodiscard]] inline Vec2 velocity_at(const WeaponRun& run, usize texel) noexcept {
    return Vec2{from_half(run.velocity[(texel * 2U) + 0]),
                from_half(run.velocity[(texel * 2U) + 1])};
}

}  // namespace cy::weapon_test
