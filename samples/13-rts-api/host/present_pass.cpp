// SPDX-License-Identifier: MIT
#include "present_pass.h"

#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/validation.h>
#include <cy/rendering/graph/executor.h>

#include "present_msl.h"
#include "present_spirv.h"

namespace sample::rts {
namespace {

namespace rhi = cy::rhi;
using cy::f32;
using cy::u32;

template <typename T, cy::usize N>
[[nodiscard]] cy::Span<const u32> words(const T (&module)[N]) noexcept {
    return cy::Span<const u32>(module, N);
}

template <cy::usize N>
[[nodiscard]] cy::Span<const cy::u8> text(const char (&module)[N]) noexcept {
    return {reinterpret_cast<const cy::u8*>(module), N - 1U};
}

[[nodiscard]] cy::Expected<rhi::ShaderModuleHandle, cy::Error> create_module(
    rhi::Device& device, const char* name, rhi::ShaderStage stage, cy::Span<const u32> spirv,
    cy::Span<const cy::u8> msl, const char* entry) noexcept {
    rhi::ShaderModuleBundle bundle;
    bundle.spirv = spirv;
    bundle.msl = msl;
    bundle.msl_entry_point = entry;
    bundle.spirv_entry_point = "main";
    rhi::ValidationMessage message;
    cy::Expected<rhi::ShaderModuleDescription, cy::Error> module = rhi::select_shader_module(
        bundle, device.capabilities().native_shader_format(), name, stage, message);
    if (!module) {
        return cy::make_unexpected(module.error());
    }
    return device.create_shader_module(*module);
}

void record_present(const cy::rendering::PassContext& context, void* user) noexcept {
    static_cast<PresentPass*>(user)->record(context);
}

}  // namespace

PresentPass::~PresentPass() {
    destroy();
}

void PresentPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (!pipeline_.is_null()) {
        device_->destroy_graphics_pipeline(pipeline_);
    }
    if (!layout_.is_null()) {
        device_->destroy_pipeline_layout(layout_);
    }
    if (!set_layout_.is_null()) {
        device_->destroy_descriptor_set_layout(set_layout_);
    }
    for (rhi::ShaderModuleHandle* module : {&vertex_, &fragment_}) {
        if (!module->is_null()) {
            device_->destroy_shader_module(*module);
        }
    }
    pipeline_ = {};
    layout_ = {};
    set_layout_ = {};
    vertex_ = {};
    fragment_ = {};
    format_ = rhi::Format::Undefined;
    device_ = nullptr;
}

cy::Status PresentPass::create(rhi::Device& device, rhi::Format format) noexcept {
    destroy();
    device_ = &device;
    format_ = format;
    auto vertex = create_module(device, "rts present vertex", rhi::ShaderStage::Vertex,
                                words(present::kPresentVertexSpirv),
                                text(present::kPresentVertexMsl), "cyRtsPresentVertex");
    auto fragment = create_module(device, "rts present fragment", rhi::ShaderStage::Fragment,
                                  words(present::kPresentFragmentSpirv),
                                  text(present::kPresentFragmentMsl), "cyRtsPresentFragment");
    if (!vertex || !fragment) {
        destroy();
        return cy::fail(cy::ErrorCode::Unsupported, "the present pass's shaders were refused");
    }
    vertex_ = *vertex;
    fragment_ = *fragment;

    rhi::DescriptorBinding binding;
    binding.binding = 0;
    binding.kind = rhi::DescriptorKind::SampledTexture;
    binding.count = 1;
    binding.stages = rhi::ShaderStage::Fragment;
    rhi::DescriptorSetLayoutDescription set;
    set.name = "rts present";
    set.bindings = cy::Span<const rhi::DescriptorBinding>(&binding, 1);
    auto made_set = device.create_descriptor_set_layout(set);
    if (!made_set) {
        destroy();
        return cy::make_unexpected(made_set.error());
    }
    set_layout_ = *made_set;

    const rhi::PushConstantRange range{rhi::ShaderStage::Fragment, 0,
                                       static_cast<u32>(sizeof(Constants))};
    rhi::PipelineLayoutDescription layout;
    layout.name = "rts present layout";
    layout.set_layouts = cy::Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    layout.push_constants = cy::Span<const rhi::PushConstantRange>(&range, 1);
    auto made_layout = device.create_pipeline_layout(layout);
    if (!made_layout) {
        destroy();
        return cy::make_unexpected(made_layout.error());
    }
    layout_ = *made_layout;

    rhi::ColorAttachmentState color;
    color.format = format;
    rhi::GraphicsPipelineDescription description;
    description.name = "rts present";
    description.layout = layout_;
    description.vertex_shader = vertex_;
    description.fragment_shader = fragment_;
    description.color_attachments = cy::Span<const rhi::ColorAttachmentState>(&color, 1);
    description.rasterisation.cull_mode = rhi::CullMode::None;
    description.depth_stencil.format = rhi::Format::Undefined;
    auto made_pipeline = device.create_graphics_pipeline(description);
    if (!made_pipeline) {
        destroy();
        return cy::make_unexpected(made_pipeline.error());
    }
    pipeline_ = *made_pipeline;
    return cy::ok();
}

void PresentPass::declare(cy::rendering::RenderGraph& graph, cy::rendering::ResourceId frame,
                          u32 frame_width, u32 frame_height, cy::rendering::ResourceId target,
                          u32 target_width, u32 target_height) noexcept {
    frame_ = frame;
    target_ = target;
    target_width_ = target_width;
    target_height_ = target_height;
    const Letterbox box =
        letterbox(static_cast<f32>(frame_width), static_cast<f32>(frame_height),
                  static_cast<f32>(target_width), static_cast<f32>(target_height));
    constants_ = Constants{};
    constants_.source[0] = frame_width;
    constants_.source[1] = frame_height;
    constants_.origin[0] = box.x;
    constants_.origin[1] = box.y;
    constants_.size[0] = box.width;
    constants_.size[1] = box.height;
    constants_.flags = rhi::format_info(format_).is_srgb ? 1U : 0U;
    graph.add_pass("rts present", rhi::QueueKind::Graphics)
        .read(frame, rhi::Access::FragmentSampledRead)
        .write(target, rhi::Access::ColorAttachmentWrite)
        .record(&record_present, this);
    // Handing the image back records nothing: what it declares is the layout presentation needs.
    graph.add_pass("rts hand to presentation", rhi::QueueKind::Graphics)
        .read(target, rhi::Access::Present)
        .side_effect();
}

void PresentPass::record(const cy::rendering::PassContext& context) noexcept {
    const cy::rendering::GraphExecutor& executor = *context.executor;
    cy::Expected<rhi::DescriptorSetHandle, cy::Error> set =
        device_->allocate_descriptor_set(set_layout_, true);
    if (!set) {
        return;
    }
    rhi::DescriptorWrite write;
    write.binding = 0;
    write.kind = rhi::DescriptorKind::SampledTexture;
    write.texture_view = executor.view(frame_);
    write.use = rhi::ImageUse::SampledRead;
    if (!device_->update_descriptor_set(*set, cy::Span<const rhi::DescriptorWrite>(&write, 1))) {
        return;
    }
    rhi::RenderAttachment attachment;
    attachment.view = executor.view(target_);
    attachment.load = rhi::LoadOp::DontCare;
    attachment.store = rhi::StoreOp::Store;
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, target_width_, target_height_};
    info.color_attachments = cy::Span<const rhi::RenderAttachment>(&attachment, 1);

    rhi::CommandBuffer& commands = *context.commands;
    commands.begin_rendering(info);
    commands.set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(target_width_),
                                        static_cast<f32>(target_height_), 0.0F, 1.0F});
    commands.set_scissor(rhi::Rect2D{0, 0, target_width_, target_height_});
    commands.bind_graphics_pipeline(pipeline_);
    commands.bind_descriptor_sets(layout_, 0, cy::Span<const rhi::DescriptorSetHandle>(&*set, 1));
    commands.push_constants(
        layout_, rhi::ShaderStage::Fragment, 0,
        cy::Span<const cy::u8>(reinterpret_cast<const cy::u8*>(&constants_), sizeof(Constants)));
    commands.draw(3, 1, 0, 0);
    commands.end_rendering();
}

}  // namespace sample::rts
