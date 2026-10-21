// SPDX-License-Identifier: MIT
// render.cpp — the RTS drawn with the device GI. See render.h.

#include "render.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/matrix.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi_gpu/gpu_frame.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>

#include "frame_scene.h"
#include "golden.h"
#include "illumination.h"
#include "rts_host.h"

namespace sample::rts {
namespace {

namespace gi_gpu = cy::rendering::gi_gpu;
using cy::f32;
using cy::f64;
using cy::u32;
using cy::u64;
using cy::Vec3;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kInstanceCount;
using cy::pipeline_test::kMaterialColours;
using cy::pipeline_test::kMaterialCount;
using cy::pipeline_test::kWidth;

/// The two slots of the frame's texture table the GI's targets are bound at.
constexpr u32 kDiffuseSlot = 118;
constexpr u32 kSpecularSlot = 119;
/// The scene's boxes after the ground: the barracks, the beacon and the crate, then the units.
constexpr u32 kFirstUnitBox = 4;
constexpr Vec3 kUnitHalf{0.3F, 0.9F, 0.3F};
constexpr Vec3 kUnitAlbedo{0.30F, 0.42F, 0.70F};
/// Where a unit box no unit stands in waits: under the ground, out of every frame.
constexpr Vec3 kParked{16.0F, -50.0F, 16.0F};
/// The frames the game reads the light at (game/Lighting.swift), and the last.
constexpr u64 kStills[] = {150, 250, 350, 420};
/// Frames before the cost is measured: pipelines and descriptor pools are made in the first.
constexpr u64 kWarmFrames = 10;
/// The beacon's foot, where the game reads the light, and the pixels round it the frame is read at.
constexpr Vec3 kBeaconFoot{12.5F, 0.05F, 24.6F};
constexpr f32 kBeaconRadiusPixels = 18.0F;

void count_validation(cy::rhi::ValidationSeverity severity, const char* message,
                      void* user) noexcept {
    if (severity == cy::rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
        std::fprintf(stderr, "graphics validation error: %s\n", message != nullptr ? message : "");
    }
}

}  // namespace

struct RtsRenderer::State {
    explicit State(cy::Allocator& allocator) noexcept : scene(allocator) {}

    RenderOptions options;
    cy::rhi::Device* device = nullptr;
    cy::rhi::BackendSelection selection{};
    u32 validation_errors = 0;
    FrameScene scene;
    gi_gpu::GpuGiFrame gi;
    RtsHost* host = nullptr;
    u64 frame = 0;

    // What the report prints.
    f64 frame_ms_total = 0.0;
    f64 frame_ms_worst = 0.0;
    f64 game_ms_total = 0.0;
    f64 update_ms_total = 0.0;
    u64 frames_timed = 0;
    f64 gi_ms_total = 0.0;
    u64 gi_timed = 0;
    f32 beacon[3] = {-1.0F, -1.0F, -1.0F};
    u32 stills = 0;

    // --- The scene's hooks --------------------------------------------------------------------

    static void configure(cy::rendering::assembly::AssemblyDescription& description,
                          void* user) noexcept {
        auto* self = static_cast<State*>(user);
        description.pin_jitter = true;
        description.global_illumination = self->options.gi;
    }

    /// The ground is the floor slab, the props are boxes one to three, the units the rest.
    static void shape_box(u32 which, Vec3& centre, Vec3& half, void*) noexcept {
        const cy::Span<const FieldBox> boxes = field_boxes();
        if (which < boxes.size()) {
            centre = boxes[which].centre;
            half = boxes[which].half;
            return;
        }
        centre = kParked;
        half = kUnitHalf;
    }

    /// The colour the GI's cards carry, through the box's material.
    static void tint_box(u32 which, Vec3& tint, void*) noexcept {
        const cy::Span<const FieldBox> boxes = field_boxes();
        const Vec3 albedo = which < boxes.size() ? boxes[which].albedo : kUnitAlbedo;
        const f32* material = kMaterialColours[which % kMaterialCount];
        tint = Vec3{albedo.x / material[0], albedo.y / material[1], albedo.z / material[2]};
    }

    /// The level's sun alone: the GI's sun, in lux.
    static void lights(cy::Array<cy::render::LightDescription>& out, void*) noexcept {
        (void)out.resize(1);
        out[0].intensity = kSunLux;
        out[0].transform.rotation =
            cy::Quat::look_rotation(cy::normalize(kSunTravel), Vec3{0.0F, 1.0F, 0.0F});
    }

    static cy::Status before_assemble(cy::rendering::RenderGraph& graph,
                                      cy::rendering::assembly::AssemblyView& view,
                                      cy::rendering::assembly::FrameSinks& sinks,
                                      void* user) noexcept {
        auto* self = static_cast<State*>(user);
        if (!self->options.gi) {
            return cy::ok();
        }
        gi_gpu::GpuGiFrameView frame;
        frame.eye = self->scene.eye();
        frame.world_to_clip = self->scene.projection() * self->scene.view();
        frame.projection = self->scene.projection();
        frame.relative_to_view = self->scene.relative_view();
        gi_gpu::GpuGiFrameTargets targets;
        if (cy::Status prepared =
                self->gi.prepare(graph, frame, self->host->illumination().frame(), targets);
            !prepared) {
            return prepared;
        }
        view.screen_space_gi = targets.diffuse;
        view.screen_space_gi_specular = targets.specular;
        sinks.screen_space_gi = self->gi.stage();
        return cy::ok();
    }

    static cy::Status before_upload(cy::rendering::pipeline::FrameUpload& upload,
                                    void* user) noexcept {
        auto* self = static_cast<State*>(user);
        if (!self->options.gi) {
            return cy::ok();
        }
        self->gi.set_jitter(
            cy::Vec2{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]});
        const cy::rendering::pipeline::MaterialTextureSlot slots[2] = {
            {kDiffuseSlot, self->gi.diffuse_view()},
            {kSpecularSlot, self->gi.specular_view()},
        };
        if (cy::Status set = self->scene.set_frame_textures({slots, 2}); !set) {
            return set;
        }
        if (self->gi.composites()) {
            self->gi.write_control(kDiffuseSlot, kSpecularSlot, upload.view.gi_control);
        }
        return cy::ok();
    }

    // --- One frame -----------------------------------------------------------------------------

    /// The mean luminance of the frame's 8-bit pixels within a few pixels of the beacon's foot.
    [[nodiscard]] f32 beacon_luminance() const noexcept {
        const cy::Vec4 clip = (scene.projection() * scene.view()) *
                              cy::Vec4{kBeaconFoot.x, kBeaconFoot.y, kBeaconFoot.z, 1.0F};
        if (clip.w <= 0.0F) {
            return -1.0F;
        }
        const f32 cx = (((clip.x / clip.w) * 0.5F) + 0.5F) * static_cast<f32>(kWidth);
        const f32 cy_ = (0.5F - ((clip.y / clip.w) * 0.5F)) * static_cast<f32>(kHeight);
        const cy::Span<const u32> pixels = scene.pixels();
        f64 sum = 0.0;
        u32 count = 0;
        for (u32 y = 0; y < kHeight; ++y) {
            for (u32 x = 0; x < kWidth; ++x) {
                const f32 dx = (static_cast<f32>(x) + 0.5F) - cx;
                const f32 dy = (static_cast<f32>(y) + 0.5F) - cy_;
                if ((dx * dx) + (dy * dy) > kBeaconRadiusPixels * kBeaconRadiusPixels) {
                    continue;
                }
                const u32 texel = pixels[(static_cast<cy::usize>(y) * kWidth) + x];
                sum += (0.2126 * static_cast<f64>(texel & 0xFFU)) +
                       (0.7152 * static_cast<f64>((texel >> 8U) & 0xFFU)) +
                       (0.0722 * static_cast<f64>((texel >> 16U) & 0xFFU));
                count += 1;
            }
        }
        return count == 0 ? -1.0F : static_cast<f32>(sum / static_cast<f64>(count));
    }

    [[nodiscard]] cy::Status write_still(u64 at) noexcept {
        cy::render_test::Image image(cy::system_allocator(cy::MemoryDomain::Renderer));
        if (cy::Status adopted = cy::render_test::adopt(image, scene.pixels(), kWidth, kHeight);
            !adopted) {
            return adopted;
        }
        char name[64];
        (void)std::snprintf(name, sizeof(name), "/rts-%s-%03llu.png", options.gi ? "gi" : "nogi",
                            static_cast<unsigned long long>(at));
        const std::string path = options.directory + name;
        if (cy::Status written = cy::render_test::write_png(path.c_str(), image); !written) {
            return written;
        }
        std::fprintf(stderr, "wrote %s\n", path.c_str());
        stills += 1;
        return cy::ok();
    }
};

RtsRenderer::RtsRenderer(cy::Allocator& allocator) noexcept : allocator_(&allocator) {}

RtsRenderer::~RtsRenderer() {
    if (state_ == nullptr) {
        return;
    }
    // THE DEVICE GOES LAST: the scene's assembly and graph release what they made through it in
    // their destructors, so the state is destroyed whole before the device is.
    cy::rhi::Device* device = state_->device;
    if (device != nullptr) {
        (void)device->wait_idle();
        state_->gi.destroy();
    }
    state_.reset();
    if (device != nullptr) {
        cy::rhi::destroy_device(cy::system_allocator(cy::MemoryDomain::Gpu), device);
    }
}

cy::Status RtsRenderer::start(RtsHost& host, const RenderOptions& options) noexcept {
    state_ = std::make_unique<State>(*allocator_);
    State& state = *state_;
    state.options = options;
    state.host = &host;
    (void)cy::rhi::vulkan::register_vulkan_backend();
    (void)cy::rhi::null::register_null_backend();
    cy::rhi::DeviceDescription description;
    description.application_name = "cy_sample_rts-api";
    description.enable_validation = true;
    cy::Allocator& gpu = cy::system_allocator(cy::MemoryDomain::Gpu);
    cy::Expected<cy::rhi::Device*, cy::Error> device =
        cy::rhi::create_device(gpu, "vulkan", description, state.selection);
    if (!device || device.value()->capabilities().backend() != cy::rhi::BackendKind::Vulkan) {
        if (device) {
            cy::rhi::destroy_device(gpu, device.value());
        }
        return cy::fail(cy::ErrorCode::Unavailable, "no requested graphics device on this machine");
    }
    state.device = device.value();
    state.device->set_validation_callback(&count_validation, &state.validation_errors);
    FrameSceneHooks hooks;
    hooks.user = &state;
    hooks.configure = &State::configure;
    hooks.shape_box = &State::shape_box;
    hooks.tint_box = &State::tint_box;
    hooks.lights = &State::lights;
    hooks.before_assemble = &State::before_assemble;
    hooks.before_upload = &State::before_upload;
    state.scene.set_hooks(hooks);
    if (cy::Status built = state.scene.build(*state.device); !built) {
        return built;
    }
    if (!options.gi) {
        return cy::ok();
    }
    if (!gi_gpu::GpuGiFrame::supported(*state.device)) {
        return cy::fail(cy::ErrorCode::Unsupported, "the device cannot run the GI");
    }
    gi_gpu::GpuGiFrameDescription gi;
    gi.width = kWidth;
    gi.height = kHeight;
    return state.gi.create(*allocator_, *state.device, host.illumination().system(), gi);
}

cy::Status RtsRenderer::frame(RtsHost& host) noexcept {
    State& state = *state_;
    // THE GAME'S CAMERA AND THE GAME'S UNITS, where this frame left them.
    state.scene.set_camera(host.camera_position(), host.camera_forward());
    for (u32 box = kFirstUnitBox; box < kInstanceCount; ++box) {
        const u32 index = box - kFirstUnitBox;
        const Vec3 at = index < host.unit_count()
                            ? host.unit_position(host.unit(index)) + Vec3{0.0F, kUnitHalf.y, 0.0F}
                            : kParked;
        if (cy::Status moved = state.scene.move_box(box, at); !moved) {
            return moved;
        }
    }
    if (cy::Status moved = state.scene.move_box(kCrateBox, host.crate_position()); !moved) {
        return moved;
    }
    state.frame = host.frames();
    const bool still = std::ranges::find(kStills, state.frame) != std::end(kStills);
    state.scene.set_read_back(still);
    cy::rendering::assembly::AssemblyReport report{};
    const auto started = std::chrono::steady_clock::now();
    if (cy::Status rendered = state.scene.render(cy::pipeline_test::RecordMode::Callbacks, report);
        !rendered) {
        return rendered;
    }
    const f64 elapsed =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    if (state.frame > kWarmFrames && !still) {
        state.frame_ms_total += elapsed;
        state.frame_ms_worst = std::max(state.frame_ms_worst, elapsed);
        state.game_ms_total += host.last_frame_ms();
        state.update_ms_total += host.illumination().last_update_ms();
        state.frames_timed += 1;
        const gi_gpu::GpuGiCost cost = state.gi.last_cost();
        if (state.options.gi && cost.any()) {
            state.gi_ms_total += cost.total_ms();
            state.gi_timed += 1;
        }
    }
    if (!still) {
        return cy::ok();
    }
    for (u32 index = 0; index < 3U; ++index) {
        if (state.frame == kStills[index]) {
            state.beacon[index] = state.beacon_luminance();
        }
    }
    return state.write_still(state.frame);
}

void RtsRenderer::print_report() const noexcept {
    if (state_ == nullptr) {
        return;
    }
    const State& state = *state_;
    const f64 frames = static_cast<f64>(std::max<u64>(state.frames_timed, 1));
    const f64 gi = static_cast<f64>(std::max<u64>(state.gi_timed, 1));
    std::printf(
        "rts render   gi=%d frames=%llu frame_ms=%.3f worst_ms=%.3f game_ms=%.3f "
        "gi_update_ms=%.3f gi_device_ms=%.3f beacon_lit=%.3f beacon_dark=%.3f beacon_relit=%.3f "
        "stills=%u validation_errors=%u\n",
        state.options.gi ? 1 : 0, static_cast<unsigned long long>(state.frame),
        state.frame_ms_total / frames, state.frame_ms_worst, state.game_ms_total / frames,
        state.update_ms_total / frames, state.gi_ms_total / gi, static_cast<f64>(state.beacon[0]),
        static_cast<f64>(state.beacon[1]), static_cast<f64>(state.beacon[2]), state.stills,
        state.validation_errors);
}

}  // namespace sample::rts
