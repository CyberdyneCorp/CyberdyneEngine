// SPDX-License-Identifier: MIT
// render.cpp — the menus drawn on a device. See render.h.

#include "render.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/post/effects.h>
#include <cy/rendering/upscale/upscale.h>
#include <cy/rendering/upscale/upscale_pass.h>
#include <cy/ui/paint.h>
#include <cy/ui/render/text_atlas.h>
#include <cy/ui/render/ui_renderer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

#include "frame_scene.h"
#include "golden.h"
#include "menus_host.h"

namespace sample::menus {
namespace {

namespace upscale = cy::rendering::upscale;
using cy::f32;
using cy::f64;
using cy::u32;
using cy::u64;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::RecordMode;

/// The scene's exposure, the pipeline suites' own.
const f32 kExposure = std::exp2(-11.4F);
/// Frames before a frame is timed: pipelines and pools are made in the first ones after a rebuild.
constexpr u64 kWarmFrames = 4;

void count_validation(cy::rhi::ValidationSeverity severity, const char* message,
                      void* user) noexcept {
    if (severity == cy::rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
        std::fprintf(stderr, "graphics validation error: %s\n", message != nullptr ? message : "");
    }
}

[[nodiscard]] upscale::UpscaleMode mode_of(u32 upscale_setting) noexcept {
    return static_cast<upscale::UpscaleMode>(
        std::min(upscale_setting, static_cast<u32>(upscale::UpscaleMode::Performance)));
}

/// Each texel of `image` the mean of the output texels it covers: a box filter over whole texels —
/// exact at an integer ratio (1920 to 480), and covering every source texel once at any other.
void average_down(cy::Span<const u32> pixels, upscale::Extent2D from,
                  cy::render_test::Image& image) noexcept {
    for (u32 y = 0; y < image.height; ++y) {
        const u32 top = y * from.height / image.height;
        const u32 bottom = std::max(top + 1, (y + 1) * from.height / image.height);
        for (u32 x = 0; x < image.width; ++x) {
            const u32 left = x * from.width / image.width;
            const u32 right = std::max(left + 1, (x + 1) * from.width / image.width);
            u32 sums[4] = {};
            for (u32 row = top; row < bottom; ++row) {
                for (u32 column = left; column < right; ++column) {
                    const u32 texel = pixels[(static_cast<std::size_t>(row) * from.width) + column];
                    for (u32 channel = 0; channel < 4; ++channel) {
                        sums[channel] += (texel >> (channel * 8U)) & 0xFFU;
                    }
                }
            }
            const u32 count = (bottom - top) * (right - left);
            u32 mean = 0;
            for (u32 channel = 0; channel < 4; ++channel) {
                mean |= ((sums[channel] + (count / 2U)) / count) << (channel * 8U);
            }
            image.texels[(static_cast<std::size_t>(y) * image.width) + x] = mean;
        }
    }
}

}  // namespace

/// One frame's worth of everything: rebuilt whole when the settings change.
struct MenusRenderer::State {
    explicit State(cy::Allocator& owner) noexcept : allocator(&owner) {}

    cy::Allocator* allocator;
    std::string directory;
    u32 shot_width = 0;
    u32 shot_height = 0;
    cy::rhi::Device* device = nullptr;
    cy::rhi::BackendSelection selection{};
    u32 validation_errors = 0;

    // The frame at the current settings.
    std::unique_ptr<FrameScene> scene;
    std::unique_ptr<upscale::TemporalUpscalePass> pass;
    std::unique_ptr<cy::ui::render::UiRenderer> ui;
    std::unique_ptr<cy::ui::PrimitiveBuffer> buffer;
    upscale::Extent2D output{};
    upscale::Extent2D render{};
    bool upscaling = false;
    cy::rendering::BloomSettings bloom{};
    bool bloom_on = false;
    u64 revision = ~0ULL;
    u32 uploaded_text = ~0U;
    u64 frames_since_build = 0;

    // The report.
    u64 frames = 0;
    u64 timed = 0;
    f64 frame_ms_total = 0.0;
    f64 frame_ms_worst = 0.0;
    f64 ui_ms_total = 0.0;
    u32 stills = 0;
    std::string extents;

    static void configure(cy::rendering::assembly::AssemblyDescription& description,
                          void* user) noexcept {
        const auto* self = static_cast<const State*>(user);
        description.pin_jitter = false;
        if (!self->upscaling) {
            return;
        }
        description.post.temporal_upscaling = true;
        description.produced_temporal_stage = true;
        description.render_width = self->render.width;
        description.render_height = self->render.height;
        description.temporal.jitter = upscale::jitter_config_for(self->output, self->render);
    }

    static cy::Status before_assemble(cy::rendering::RenderGraph&,
                                      cy::rendering::assembly::AssemblyView&,
                                      cy::rendering::assembly::FrameSinks& sinks,
                                      void* user) noexcept {
        auto* self = static_cast<State*>(user);
        if (self->upscaling) {
            sinks.temporal = upscale::stage_of(*self->pass);
        }
        sinks.ui = self->ui->stage();
        return cy::ok();
    }

    void release() noexcept {
        if (device != nullptr) {
            (void)device->wait_idle();
        }
        if (ui != nullptr) {
            ui->destroy();
        }
        if (pass != nullptr) {
            pass->destroy();
        }
        if (scene != nullptr) {
            scene->release();
        }
        ui.reset();
        pass.reset();
        scene.reset();
        uploaded_text = ~0U;
    }

    /// The frame again, at `settings`.
    [[nodiscard]] cy::Status build(const CyVideoSettings& settings) noexcept {
        release();
        output = {settings.width, settings.height};
        const upscale::UpscaleMode mode = mode_of(settings.upscale);
        upscaling = mode != upscale::UpscaleMode::NativeAntialiasing;
        render = upscaling ? upscale::render_extent_for(output, upscale::upscale_mode_scale(mode))
                           : output;
        bloom_on = settings.quality != CY_VIDEO_QUALITY_LOW;
        bloom.mip_count = settings.quality == CY_VIDEO_QUALITY_MEDIUM ? 4U : 6U;
        scene = std::make_unique<FrameScene>(*allocator);
        FrameSceneHooks hooks;
        hooks.user = this;
        hooks.configure = &State::configure;
        hooks.before_assemble = &State::before_assemble;
        scene->set_hooks(hooks);
        scene->set_extent(output.width, output.height);
        if (upscaling) {
            pass = std::make_unique<upscale::TemporalUpscalePass>();
            upscale::UpscalePassDescription description;
            description.output = output;
            if (cy::Status made = pass->create(*allocator, *device, description); !made) {
                return made;
            }
            upscale::UpscaleSettings upscale_settings;
            upscale_settings.mode = mode;
            upscale_settings.exposure = kExposure;
            pass->set_settings(upscale_settings);
        }
        if (cy::Status built = scene->build(*device, bloom_on ? &bloom : nullptr); !built) {
            return built;
        }
        scene->set_read_back(true);
        if (upscaling) {
            if (cy::Status bound = pass->bind_temporal(scene->assembly().temporal()); !bound) {
                return bound;
            }
        }
        ui = std::make_unique<cy::ui::render::UiRenderer>(*allocator);
        cy::ui::render::UiRendererDescription description;
        description.width = output.width;
        description.height = output.height;
        description.output_format = scene->pipelines().setup().output_format;
        if (cy::Status made = ui->create(*device, description); !made) {
            return made;
        }
        if (buffer == nullptr) {
            buffer = std::make_unique<cy::ui::PrimitiveBuffer>(*allocator);
        }
        frames_since_build = 0;
        char extent[64] = {};
        (void)std::snprintf(extent, sizeof(extent), "%s%ux%u@%ux%u", extents.empty() ? "" : ",",
                            output.width, output.height, render.width, render.height);
        extents += extent;
        return cy::ok();
    }

    /// Flatten the host's interface at the output's scale and hand it to the renderer.
    [[nodiscard]] cy::Status submit_interface(MenusHost& host) noexcept {
        cy::ui::ScaleSettings scale;
        scale.mode = cy::ui::ScaleMode::ScaleWithSmaller;
        scale.reference =
            cy::Vec2{static_cast<f32>(kReferenceWidth), static_cast<f32>(kReferenceHeight)};
        const cy::Vec2 viewport{static_cast<f32>(output.width), static_cast<f32>(output.height)};
        const f32 factor = cy::ui::resolve_scale(scale, viewport);
        cy::ui::FlattenReport flattened;
        if (cy::Status flat = cy::ui::flatten(
                host.ui_store(), cy::ui::Rect{0.0F, 0.0F, viewport.x / factor, viewport.y / factor},
                *buffer, flattened, &host.ui_text());
            !flat) {
            return flat;
        }
        if (host.ui_text().atlas_revision() != uploaded_text) {
            if (cy::Status uploaded = cy::ui::render::upload_text_atlases(*ui, host.ui_text());
                !uploaded) {
                return uploaded;
            }
            uploaded_text = host.ui_text().atlas_revision();
        }
        return ui->submit(*buffer, factor);
    }

    [[nodiscard]] cy::Status save(const std::string& name) noexcept {
        cy::render_test::Image image(*allocator);
        const bool resized = shot_width != 0 && shot_height != 0;
        image.width = resized ? shot_width : output.width;
        image.height = resized ? shot_height : output.height;
        if (cy::Status sized =
                image.texels.resize(static_cast<std::size_t>(image.width) * image.height);
            !sized) {
            return sized;
        }
        average_down(scene->pixels(), output, image);
        const std::string path = directory + "/" + name + ".png";
        if (cy::Status written = cy::render_test::write_png(path.c_str(), image); !written) {
            return written;
        }
        ++stills;
        return cy::ok();
    }
};

MenusRenderer::MenusRenderer(cy::Allocator& allocator) noexcept : allocator_(&allocator) {}

MenusRenderer::~MenusRenderer() {
    if (state_ != nullptr) {
        state_->release();
        if (state_->device != nullptr) {
            cy::rhi::destroy_device(cy::system_allocator(cy::MemoryDomain::Gpu), state_->device);
        }
    }
}

cy::Status MenusRenderer::start(MenusHost& host, std::string directory, u32 shot_width,
                                u32 shot_height) noexcept {
    state_ = std::make_unique<State>(*allocator_);
    state_->directory = std::move(directory);
    state_->shot_width = shot_width;
    state_->shot_height = shot_height;
    (void)cy::rhi::vulkan::register_vulkan_backend();
    (void)cy::rhi::null::register_null_backend();
    cy::rhi::DeviceDescription description;
    description.application_name = "cy_sample_menus";
    description.enable_validation = true;
    cy::Allocator& gpu = cy::system_allocator(cy::MemoryDomain::Gpu);
    cy::Expected<cy::rhi::Device*, cy::Error> device =
        cy::rhi::create_device(gpu, "vulkan", description, state_->selection);
    if (!device || device.value()->capabilities().backend() != cy::rhi::BackendKind::Vulkan) {
        if (device) {
            cy::rhi::destroy_device(gpu, device.value());
        }
        return cy::fail(cy::ErrorCode::Unavailable, "no requested graphics device on this machine");
    }
    state_->device = device.value();
    state_->device->set_validation_callback(&count_validation, &state_->validation_errors);
    state_->revision = host.video().revision();
    return state_->build(host.video().current());
}

cy::Status MenusRenderer::frame(MenusHost& host, const std::vector<std::string>& shots) noexcept {
    State& state = *state_;
    if (host.video().revision() != state.revision) {
        state.revision = host.video().revision();
        if (cy::Status rebuilt = state.build(host.video().current()); !rebuilt) {
            return rebuilt;
        }
    }
    const auto started = std::chrono::steady_clock::now();
    if (cy::Status submitted = state.submit_interface(host); !submitted) {
        return submitted;
    }
    const f64 ui_ms =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    cy::rendering::assembly::AssemblyReport report{};
    if (cy::Status rendered = state.scene->render(RecordMode::Callbacks, report); !rendered) {
        return rendered;
    }
    const f64 frame_ms =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count() +
        host.last_frame_ms();
    ++state.frames;
    if (++state.frames_since_build > kWarmFrames) {
        ++state.timed;
        state.frame_ms_total += frame_ms;
        state.frame_ms_worst = std::max(state.frame_ms_worst, frame_ms);
        state.ui_ms_total += ui_ms;
    }
    for (const std::string& name : shots) {
        if (cy::Status saved = state.save(name); !saved) {
            return saved;
        }
    }
    return cy::ok();
}

void MenusRenderer::print_report() const noexcept {
    if (state_ == nullptr) {
        return;
    }
    const State& state = *state_;
    const f64 timed = state.timed == 0 ? 1.0 : static_cast<f64>(state.timed);
    std::printf(
        "menus render: frames=%llu timed=%llu frame_ms=%.3f worst_ms=%.3f ui_submit_ms=%.3f "
        "stills=%u validation=%u extents=%s\n",
        static_cast<unsigned long long>(state.frames), static_cast<unsigned long long>(state.timed),
        state.frame_ms_total / timed, state.frame_ms_worst, state.ui_ms_total / timed, state.stills,
        state.validation_errors, state.extents.c_str());
}

}  // namespace sample::menus
