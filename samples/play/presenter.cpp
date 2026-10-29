// SPDX-License-Identifier: MIT
// presenter.cpp — see presenter.h.

#include "presenter.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/validation.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/ui/layout.h>
#include <cy/ui/paint.h>
#include <cy/ui/render/text_atlas.h>
#include <cy/ui/render/ui_renderer.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "play_ui.h"
#include "present_msl.h"
#include "present_spirv.h"

namespace cy::play {
namespace {

namespace rendering = cy::rendering;
using rendering::ResourceId;
using rhi::Access;
using rhi::QueueKind;

/// `kCyPresentDecode` in present.slang.
constexpr u32 kPresentDecode = 1U;

struct PresentPush {
    u32 flags = 0;
    u32 pad[3] = {};
};
static_assert(sizeof(PresentPush) == 16, "present.slang's push block is sixteen bytes");

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u64*>(user);
        std::fprintf(stderr, "play: graphics validation error: %s\n",
                     message != nullptr ? message : "");
    }
}

[[nodiscard]] bool is_srgb(rhi::Format format) noexcept {
    return format == rhi::Format::Bgra8Srgb || format == rhi::Format::Rgba8Srgb;
}

[[nodiscard]] bool is_bgra(rhi::Format format) noexcept {
    return format == rhi::Format::Bgra8Srgb || format == rhi::Format::Bgra8Unorm;
}

template <usize N>
[[nodiscard]] Span<const u32> words(const u32 (&module)[N]) noexcept {
    return Span<const u32>(module, N);
}

template <usize N>
[[nodiscard]] Span<const u8> text(const char (&module)[N]) noexcept {
    return {reinterpret_cast<const u8*>(module), N - 1U};
}

[[nodiscard]] Expected<rhi::ShaderModuleHandle, Error> make_module(
    rhi::Device& device, const char* name, rhi::ShaderStage stage, Span<const u32> spirv,
    Span<const u8> msl, const char* entry) noexcept {
    rhi::ShaderModuleBundle bundle;
    bundle.spirv = spirv;
    bundle.msl = msl;
    bundle.msl_entry_point = entry;
    bundle.spirv_entry_point = "main";
    rhi::ValidationMessage message;
    Expected<rhi::ShaderModuleDescription, Error> module = rhi::select_shader_module(
        bundle, device.capabilities().native_shader_format(), name, stage, message);
    if (!module) {
        return make_unexpected(module.error());
    }
    return device.create_shader_module(*module);
}

/// The picture's rectangle in a target of `target_width` x `target_height`: as large as fits, its
/// aspect kept, centred.
[[nodiscard]] rhi::Rect2D letterbox(u32 picture_width, u32 picture_height, u32 target_width,
                                    u32 target_height) noexcept {
    const f64 scale_x = static_cast<f64>(target_width) / static_cast<f64>(picture_width);
    const f64 scale_y = static_cast<f64>(target_height) / static_cast<f64>(picture_height);
    const f64 scale = scale_x < scale_y ? scale_x : scale_y;
    const auto width = static_cast<u32>(std::lround(static_cast<f64>(picture_width) * scale));
    const auto height = static_cast<u32>(std::lround(static_cast<f64>(picture_height) * scale));
    rhi::Rect2D rect;
    rect.width = width < target_width ? width : target_width;
    rect.height = height < target_height ? height : target_height;
    rect.x = static_cast<i32>((target_width - rect.width) / 2U);
    rect.y = static_cast<i32>((target_height - rect.height) / 2U);
    return rect;
}

}  // namespace

struct Presenter::State {
    explicit State(Allocator& memory) noexcept
        : allocator(&memory), interface(memory), buffer(memory) {}

    Allocator* allocator;
    PresenterOptions options;
    DisplayServer* display = nullptr;
    WindowId window = kInvalidWindow;
    PlayUi* ui = nullptr;
    rhi::Device* device = nullptr;
    NativeSurface surface;
    bool surface_made = false;
    rhi::SwapchainHandle swapchain;
    rhi::SwapchainInfo info;
    rhi::SemaphoreHandle acquired;
    rhi::SemaphoreHandle presented;
    rhi::ShaderModuleHandle vertex;
    rhi::ShaderModuleHandle fragment;
    rhi::DescriptorSetLayoutHandle set_layout;
    rhi::PipelineLayoutHandle layout;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::SamplerHandle sampler;
    rhi::BufferHandle readback;
    u64 readback_size = 0;
    u64 validation_errors = 0;
    ui::render::UiRenderer interface;
    ui::PrimitiveBuffer buffer;
    u64 atlas_revision = ~u64{0};

    // What the present pass's record callback reads.
    ResourceId picture = rendering::kInvalidResource;
    ResourceId target = rendering::kInvalidResource;
    ResourceId host_copy = rendering::kInvalidResource;
    PresentSource source;

    [[nodiscard]] Status create_device() noexcept {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = options.application;
        description.enable_validation = options.validation;
        rhi::BackendSelection selection;
        Allocator& gpu = system_allocator(MemoryDomain::Gpu);
        Expected<rhi::Device*, Error> made =
            rhi::create_device(gpu, "vulkan", description, selection);
        if (!made || made.value()->capabilities().backend() != rhi::BackendKind::Vulkan) {
            if (made) {
                rhi::destroy_device(gpu, made.value());
            }
            return fail(ErrorCode::Unavailable, "no requested graphics device on this machine");
        }
        device = *made;
        device->set_validation_callback(&count_validation, &validation_errors);
        return ok();
    }

    [[nodiscard]] Status create_swapchain(Extent extent) noexcept {
        if (!display->has_feature(Feature::VulkanSurface)) {
            return fail(ErrorCode::Unavailable, "the display server has no Vulkan surface");
        }
        SurfaceDescription description;
        description.api = GraphicsApi::Vulkan;
        description.api_instance = rhi::vulkan::vulkan_instance_handle(*device);
        Expected<NativeSurface, Error> made = display->create_surface(window, description);
        if (!made) {
            return make_unexpected(made.error());
        }
        surface = *made;
        surface_made = true;
        rhi::SwapchainDescription swapchain_description;
        swapchain_description.name = "play";
        swapchain_description.native_surface = surface.handle;
        swapchain_description.extent =
            rhi::Extent2D{static_cast<u32>(extent.width), static_cast<u32>(extent.height)};
        // UNORM, because the picture holds encoded bytes; an sRGB answer is handled by decoding.
        swapchain_description.preferred_format = rhi::Format::Bgra8Unorm;
        // V-SYNC OFF PRESENTS AT ONCE, tearing allowed: the frame's own cost is what is measured,
        // never the display's interval. FIFO is the fallback every implementation has.
        swapchain_description.present_mode =
            options.vsync ? rhi::PresentMode::Fifo : rhi::PresentMode::Immediate;
        Expected<rhi::SwapchainHandle, Error> chain =
            device->create_swapchain(swapchain_description);
        if (!chain) {
            return make_unexpected(chain.error());
        }
        swapchain = *chain;
        info = device->swapchain_info(swapchain);
        Expected<rhi::SemaphoreHandle, Error> first = device->create_semaphore();
        Expected<rhi::SemaphoreHandle, Error> second = device->create_semaphore();
        if (!first || !second) {
            return fail(ErrorCode::OutOfMemory, "presentation's semaphores were refused");
        }
        acquired = *first;
        presented = *second;
        return ok();
    }

    [[nodiscard]] Status create_pipeline() noexcept {
        Expected<rhi::ShaderModuleHandle, Error> module =
            make_module(*device, "play present vertex", rhi::ShaderStage::Vertex,
                        words(kPresentVertexSpirv), text(kPresentVertexMsl), "cyPresentVertex");
        if (!module) {
            return make_unexpected(module.error());
        }
        vertex = *module;
        module = make_module(*device, "play present fragment", rhi::ShaderStage::Fragment,
                             words(kPresentFragmentSpirv), text(kPresentFragmentMsl),
                             "cyPresentFragment");
        if (!module) {
            return make_unexpected(module.error());
        }
        fragment = *module;
        rhi::DescriptorBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].kind = rhi::DescriptorKind::SampledTexture;
        bindings[0].count = 1;
        bindings[0].stages = rhi::ShaderStage::Fragment;
        bindings[1].binding = 1;
        bindings[1].kind = rhi::DescriptorKind::Sampler;
        bindings[1].count = 1;
        bindings[1].stages = rhi::ShaderStage::Fragment;
        rhi::DescriptorSetLayoutDescription set;
        set.name = "play present";
        set.bindings = Span<const rhi::DescriptorBinding>(bindings, 2);
        Expected<rhi::DescriptorSetLayoutHandle, Error> made_set =
            device->create_descriptor_set_layout(set);
        if (!made_set) {
            return make_unexpected(made_set.error());
        }
        set_layout = *made_set;
        const rhi::PushConstantRange range{rhi::ShaderStage::Fragment, 0,
                                           static_cast<u32>(sizeof(PresentPush))};
        rhi::PipelineLayoutDescription layout_description;
        layout_description.name = "play present layout";
        layout_description.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout, 1);
        layout_description.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
        Expected<rhi::PipelineLayoutHandle, Error> made_layout =
            device->create_pipeline_layout(layout_description);
        if (!made_layout) {
            return make_unexpected(made_layout.error());
        }
        layout = *made_layout;
        rhi::ColorAttachmentState color;
        color.format = info.format;
        rhi::GraphicsPipelineDescription description;
        description.name = "play present";
        description.layout = layout;
        description.vertex_shader = vertex;
        description.fragment_shader = fragment;
        description.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
        description.rasterisation.cull_mode = rhi::CullMode::None;
        description.depth_stencil.format = rhi::Format::Undefined;
        Expected<rhi::GraphicsPipelineHandle, Error> made_pipeline =
            device->create_graphics_pipeline(description);
        if (!made_pipeline) {
            return make_unexpected(made_pipeline.error());
        }
        pipeline = *made_pipeline;
        rhi::SamplerDescription filter;
        filter.name = "play present linear clamp";
        filter.min_filter = rhi::Filter::Linear;
        filter.mag_filter = rhi::Filter::Linear;
        filter.address_u = rhi::AddressMode::ClampToEdge;
        filter.address_v = rhi::AddressMode::ClampToEdge;
        filter.address_w = rhi::AddressMode::ClampToEdge;
        Expected<rhi::SamplerHandle, Error> made_sampler = device->create_sampler(filter);
        if (!made_sampler) {
            return make_unexpected(made_sampler.error());
        }
        sampler = *made_sampler;
        return ok();
    }

    /// The interface renderer at the swapchain's extent, and the text's pages on it.
    [[nodiscard]] Status create_interface() noexcept {
        interface.destroy();
        if (ui == nullptr || info.extent.width == 0U || info.extent.height == 0U) {
            return ok();  // no window to draw it in yet: made when the window has a size
        }
        ui::render::UiRendererDescription description;
        description.width = info.extent.width;
        description.height = info.extent.height;
        description.output_format = info.format;
        if (Status made = interface.create(*device, description); !made) {
            return made;
        }
        atlas_revision = ~u64{0};
        buffer.clear();
        return ok();
    }

    [[nodiscard]] Status create_readback() noexcept {
        const u64 size = u64{info.extent.width} * info.extent.height * 4U;
        if (size == readback_size || size == 0U) {
            return ok();
        }
        if (!readback.is_null()) {
            device->destroy_buffer(readback);
            readback = rhi::BufferHandle{};
        }
        rhi::BufferDescription description;
        description.name = "play capture";
        description.size = size;
        description.usage = rhi::BufferUsage::TransferDestination;
        description.memory = rhi::MemoryUse::Readback;
        Expected<rhi::BufferHandle, Error> made = device->create_buffer(description);
        if (!made) {
            return make_unexpected(made.error());
        }
        readback = *made;
        readback_size = size;
        return ok();
    }

    /// The interface's text pages, when the painter rasterised something new. Outside a frame:
    /// `upload_text_atlases` runs a graph of its own.
    [[nodiscard]] Status refresh_atlases() noexcept {
        if (ui == nullptr || !interface.ready()) {
            return ok();
        }
        const u64 revision = ui->text().atlas_revision();
        if (revision == atlas_revision) {
            return ok();
        }
        atlas_revision = revision;
        return ui::render::upload_text_atlases(interface, ui->text());
    }

    /// The interface flattened and handed to its renderer for this frame.
    [[nodiscard]] Status submit_interface() noexcept {
        if (ui == nullptr || !interface.ready()) {
            return ok();
        }
        if (Status settled = ui->settle(); !settled) {
            return settled;
        }
        const Vec2 viewport = ui->viewport();
        ui::FlattenReport flattened{};
        if (Status flat = ui::flatten(ui->store(), ui::Rect{0.0F, 0.0F, viewport.x, viewport.y},
                                      buffer, flattened, &ui->text());
            !flat) {
            return flat;
        }
        const f32 scale =
            viewport.x > 0.0F ? static_cast<f32>(info.extent.width) / viewport.x : 1.0F;
        return interface.submit(buffer, scale);
    }

    static void record_present(const rendering::PassContext& context, void* user) noexcept {
        auto* self = static_cast<State*>(user);
        rhi::Device& gpu = *self->device;
        rhi::RenderAttachment attachment;
        attachment.view = context.executor->view(self->target);
        attachment.load = rhi::LoadOp::Clear;
        attachment.store = rhi::StoreOp::Store;
        attachment.clear.color[3] = 1.0F;
        rhi::RenderingInfo rendering_info;
        rendering_info.render_area =
            rhi::Rect2D{0, 0, self->info.extent.width, self->info.extent.height};
        rendering_info.color_attachments = Span<const rhi::RenderAttachment>(&attachment, 1);
        Expected<rhi::DescriptorSetHandle, Error> set =
            gpu.allocate_descriptor_set(self->set_layout, true);
        if (!set) {
            return;
        }
        rhi::DescriptorWrite writes[2] = {};
        writes[0].binding = 0;
        writes[0].kind = rhi::DescriptorKind::SampledTexture;
        writes[0].texture_view = context.executor->view(self->picture);
        writes[0].use = rhi::ImageUse::SampledRead;
        writes[1].binding = 1;
        writes[1].kind = rhi::DescriptorKind::Sampler;
        writes[1].sampler = self->sampler;
        if (!gpu.update_descriptor_set(*set, Span<const rhi::DescriptorWrite>(writes, 2))) {
            return;
        }
        const rhi::Rect2D box = letterbox(self->source.width, self->source.height,
                                          self->info.extent.width, self->info.extent.height);
        rhi::Viewport viewport;
        viewport.x = static_cast<f32>(box.x);
        viewport.y = static_cast<f32>(box.y);
        viewport.width = static_cast<f32>(box.width);
        viewport.height = static_cast<f32>(box.height);
        PresentPush push;
        push.flags = is_srgb(self->info.format) ? kPresentDecode : 0U;
        rhi::CommandBuffer& commands = *context.commands;
        commands.begin_rendering(rendering_info);
        commands.bind_graphics_pipeline(self->pipeline);
        commands.set_viewport(viewport);
        commands.set_scissor(box);
        commands.bind_descriptor_sets(self->layout, 0,
                                      Span<const rhi::DescriptorSetHandle>(&*set, 1));
        commands.push_constants(self->layout, rhi::ShaderStage::Fragment, 0,
                                Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(push)));
        commands.draw(3, 1, 0, 0);
        commands.end_rendering();
    }

    static void record_capture(const rendering::PassContext& context, void* user) noexcept {
        auto* self = static_cast<State*>(user);
        rhi::BufferTextureCopy region;
        region.buffer_row_length = self->info.extent.width;
        region.texture_extent = rhi::Extent3D{self->info.extent.width, self->info.extent.height, 1};
        context.commands->copy_texture_to_buffer(context.executor->texture(self->target),
                                                 self->readback,
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }

    /// The frame's graph: the picture, the interface, the capture, the hand-back.
    [[nodiscard]] Status declare(rendering::RenderGraph& graph, u32 image, bool capture) noexcept {
        rendering::TextureRequest picture_request;
        picture_request.name = "play picture";
        picture_request.format = source.format;
        picture_request.width = source.width;
        picture_request.height = source.height;
        picture = graph.import_texture(picture_request, source.texture, source.use);
        rendering::TextureRequest target_request;
        target_request.name = "play swapchain";
        target_request.format = info.format;
        target_request.width = info.extent.width;
        target_request.height = info.extent.height;
        target_request.extra_usage =
            rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSource;
        target = graph.import_swapchain_texture(target_request,
                                                device->swapchain_texture(swapchain, image));
        graph.add_pass("play present", QueueKind::Graphics)
            .read(picture, Access::FragmentSampledRead)
            .write(target, Access::ColorAttachmentWrite)
            .record(&State::record_present, this);
        if (ui != nullptr && interface.ready()) {
            rendering::ScreenSpaceStageInputs inputs;
            inputs.target = target;
            inputs.width = info.extent.width;
            inputs.height = info.extent.height;
            if (interface.declare(graph, inputs) == rendering::kInvalidPass) {
                return fail(ErrorCode::Internal, "the interface pass was refused");
            }
        }
        if (capture) {
            rendering::BufferRequest copy_request;
            copy_request.name = "play capture";
            copy_request.size = readback_size;
            copy_request.extra_usage = rhi::BufferUsage::TransferDestination;
            host_copy = graph.import_buffer(copy_request, readback);
            graph.add_pass("play capture", QueueKind::Graphics)
                .read(target, Access::TransferRead)
                .write(host_copy, Access::TransferWrite)
                .record(&State::record_capture, this);
            graph.add_pass("play capture host", QueueKind::Graphics)
                .read(host_copy, Access::HostRead)
                .side_effect();
        }
        graph.add_pass("play hand back", QueueKind::Graphics)
            .read(target, Access::Present)
            .side_effect();
        return graph.status();
    }
};

Presenter::Presenter(Allocator& allocator) noexcept : allocator_(&allocator) {}

Presenter::~Presenter() {
    shutdown();
}

Status Presenter::start(DisplayServer& display, WindowId window, PlayUi* ui,
                        const PresenterOptions& options) noexcept {
    state_ = std::make_unique<State>(*allocator_);
    State& state = *state_;
    state.options = options;
    state.display = &display;
    state.window = window;
    state.ui = ui;
    if (Status made = state.create_device(); !made) {
        state_.reset();
        return made;
    }
    device_ = state.device;
    const Expected<Extent, Error> size = display.window_size(window);
    if (!size) {
        return make_unexpected(size.error());
    }
    using Step = Status (State::*)() noexcept;
    if (Status chain = state.create_swapchain(*size); !chain) {
        return chain;
    }
    for (const Step step :
         {&State::create_pipeline, &State::create_interface, &State::create_readback}) {
        if (Status made = (state.*step)(); !made) {
            return made;
        }
    }
    return ok();
}

rhi::SwapchainInfo Presenter::info() const noexcept {
    return state_ == nullptr ? rhi::SwapchainInfo{} : state_->info;
}

u64 Presenter::validation_errors() const noexcept {
    return state_ == nullptr ? 0U : state_->validation_errors;
}

const char* Presenter::device_name() const noexcept {
    return device_ == nullptr ? "" : device_->capabilities().device_name();
}

Status Presenter::resize(Extent extent) noexcept {
    State& state = *state_;
    if (extent.width <= 0 || extent.height <= 0) {
        return ok();  // minimised: nothing to present into until it is restored
    }
    if (Status idle = device_->wait_idle(); !idle) {
        return idle;
    }
    if (Status resized = device_->resize_swapchain(
            state.swapchain,
            rhi::Extent2D{static_cast<u32>(extent.width), static_cast<u32>(extent.height)});
        !resized) {
        return resized;
    }
    state.info = device_->swapchain_info(state.swapchain);
    if (Status made = state.create_interface(); !made) {
        return made;
    }
    return state.create_readback();
}

Status Presenter::follow_window() noexcept {
    const Expected<Extent, Error> size = state_->display->window_size(state_->window);
    if (!size) {
        return make_unexpected(size.error());
    }
    return resize(*size);
}

Status Presenter::draw(u32 image, bool capture, PresentResult& result) noexcept {
    State& state = *state_;
    Status executed = state.submit_interface();
    rendering::RenderGraph graph(*state.allocator);
    if (executed) {
        executed = state.declare(graph, image, capture);
    }
    rendering::GraphExecutor executor(*state.allocator, *device_);
    if (executed) {
        rendering::ExecuteOptions options;
        options.wait_acquire = state.acquired;
        options.signal_present = state.presented;
        const Expected<rendering::ExecutionResult, Error> ran =
            executor.execute(graph, rendering::CompileOptions{}, options);
        executed = ran ? ok() : make_unexpected(ran.error());
    }
    if (executed) {
        result.presented = device_->present(state.swapchain, image, state.presented).has_value();
        executed = device_->wait_idle();
    }
    executor.release();
    return executed;
}

void Presenter::read_capture() noexcept {
    const State& state = *state_;
    const auto* bytes = static_cast<const u8*>(device_->buffer_mapped_pointer(state.readback));
    captured_width_ = state.info.extent.width;
    captured_height_ = state.info.extent.height;
    captured_.assign(static_cast<usize>(captured_width_) * captured_height_, 0U);
    if (bytes == nullptr) {
        return;
    }
    const bool swap = is_bgra(state.info.format);
    for (usize texel = 0; texel < captured_.size(); ++texel) {
        const u8* from = bytes + (texel * 4U);
        const u32 r = swap ? from[2] : from[0];
        const u32 b = swap ? from[0] : from[2];
        captured_[texel] = r | (u32{from[1]} << 8U) | (b << 16U) | 0xFF000000U;
    }
}

Expected<bool, Error> Presenter::present_once(bool capture, PresentResult& result) noexcept {
    if (Expected<u32, Error> began = device_->begin_frame(); !began) {
        return make_unexpected(began.error());
    }
    const Expected<u32, Error> image =
        device_->acquire_next_image(state_->swapchain, state_->acquired, 1'000'000'000ULL);
    const Status drawn = image ? draw(*image, capture, result) : ok();
    (void)device_->end_frame();
    if (!drawn) {
        return make_unexpected(drawn.error());
    }
    if (result.presented) {
        return true;
    }
    // OUT OF DATE: asked what the window is now, rather than told what it was.
    if (Status followed = follow_window(); !followed) {
        return make_unexpected(followed.error());
    }
    result.resized = true;
    return false;
}

Expected<PresentResult, Error> Presenter::present(const PresentSource& source,
                                                  bool capture) noexcept {
    State& state = *state_;
    PresentResult result;
    state.source = source;
    if (state.info.extent.width == 0U || state.info.extent.height == 0U) {
        // A window that had no size when the swapchain was made — a fullscreen one before the
        // window manager placed it — is asked again; a minimised one presents nothing.
        if (Status followed = follow_window(); !followed) {
            return make_unexpected(followed.error());
        }
        if (state.info.extent.width == 0U || state.info.extent.height == 0U) {
            return result;
        }
    }
    if (Status atlases = state.refresh_atlases(); !atlases) {
        return make_unexpected(atlases.error());
    }
    const auto started = std::chrono::steady_clock::now();
    // A window resized between the frame's events and its present leaves the swapchain out of
    // date at the acquire or at the present. The swapchain follows the window and the frame is
    // drawn again into it, once: the game's picture is still there, so a resize drops no frame.
    Expected<bool, Error> shown = present_once(capture, result);
    const bool sized = state.info.extent.width != 0U && state.info.extent.height != 0U;
    if (shown && !*shown && sized) {
        shown = present_once(capture, result);
    }
    if (!shown) {
        return make_unexpected(shown.error());
    }
    if (capture && result.presented) {
        read_capture();
    }
    result.present_ms =
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - started).count();
    return result;
}

void Presenter::release() noexcept {
    if (state_ == nullptr || device_ == nullptr) {
        return;
    }
    State& state = *state_;
    (void)device_->wait_idle();
    state.interface.destroy();
    if (!state.readback.is_null()) {
        device_->destroy_buffer(state.readback);
        state.readback = rhi::BufferHandle{};
    }
    if (!state.sampler.is_null()) {
        device_->destroy_sampler(state.sampler);
        state.sampler = rhi::SamplerHandle{};
    }
    if (!state.pipeline.is_null()) {
        device_->destroy_graphics_pipeline(state.pipeline);
        state.pipeline = rhi::GraphicsPipelineHandle{};
    }
    if (!state.layout.is_null()) {
        device_->destroy_pipeline_layout(state.layout);
        state.layout = rhi::PipelineLayoutHandle{};
    }
    if (!state.set_layout.is_null()) {
        device_->destroy_descriptor_set_layout(state.set_layout);
        state.set_layout = rhi::DescriptorSetLayoutHandle{};
    }
    for (rhi::ShaderModuleHandle* module : {&state.vertex, &state.fragment}) {
        if (!module->is_null()) {
            device_->destroy_shader_module(*module);
            *module = rhi::ShaderModuleHandle{};
        }
    }
    for (rhi::SemaphoreHandle* semaphore : {&state.acquired, &state.presented}) {
        if (!semaphore->is_null()) {
            device_->destroy_semaphore(*semaphore);
            *semaphore = rhi::SemaphoreHandle{};
        }
    }
    if (!state.swapchain.is_null()) {
        device_->destroy_swapchain(state.swapchain);
        state.swapchain = rhi::SwapchainHandle{};
    }
    if (state.surface_made) {
        state.display->destroy_surface(state.surface);
        state.surface_made = false;
    }
}

void Presenter::shutdown() noexcept {
    release();
    if (device_ != nullptr) {
        rhi::destroy_device(system_allocator(MemoryDomain::Gpu), device_);
        device_ = nullptr;
    }
    state_.reset();
}

}  // namespace cy::play
