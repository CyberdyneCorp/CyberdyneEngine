// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_PRESENT_PASS_H
#define CY_SAMPLE_RTS_API_PRESENT_PASS_H
// present_pass.h — `--play`'s last pass: the frame scaled into the window's swapchain image.
//
// The frame is drawn at an extent fixed when the renderer starts (map_render.h): its targets, its
// GI, its fog and its HUD are sized once. The window is the player's to resize. So the frame is not
// drawn INTO the swapchain image; this pass draws it ONTO it, scaled to fit and centred, with black
// bars where the window's aspect is not the frame's — the letterbox — in one full-screen triangle
// (shaders/present.slang). The same letterbox is what the platform's pointer mapping inverts
// (`cy::PointerMapping`), so a click lands on the pixel of the frame it was made over.
//
// The swapchain image is imported as one (`RenderGraph::import_swapchain_texture`) and handed to
// presentation by a declared `Access::Present` read, so the graph derives every barrier, and the
// graph's executor waits for the acquire and signals the present (`ExecuteOptions`). That graph is
// its own, after the frame's (map_render.cpp), so the frame's cost never includes waiting for the
// presentation engine to hand an image back.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/graph/graph.h>

#include "letterbox.h"

namespace sample::rts {

/// The swapchain image a frame is presented to, acquired by the caller for this frame.
struct PresentTarget {
    cy::rhi::TextureHandle image;
    cy::rhi::Format format = cy::rhi::Format::Undefined;
    cy::u32 width = 0;
    cy::u32 height = 0;
    /// Waited before the image is written, and signalled once it is ready to present.
    cy::rhi::SemaphoreHandle acquired;
    cy::rhi::SemaphoreHandle presented;
};

class PresentPass {
public:
    PresentPass() = default;
    ~PresentPass();

    PresentPass(const PresentPass&) = delete;
    PresentPass& operator=(const PresentPass&) = delete;

    /// The pipeline for a swapchain of `format`. Again with another format replaces it.
    [[nodiscard]] cy::Status create(cy::rhi::Device& device, cy::rhi::Format format) noexcept;
    void destroy() noexcept;
    [[nodiscard]] cy::rhi::Format format() const noexcept { return format_; }

    /// Declare the pass: `frame` (`frame_width` by `frame_height`, sampled) drawn letterboxed onto
    /// `target` (the swapchain image, `target_width` by `target_height`), and the image then handed
    /// to presentation.
    void declare(cy::rendering::RenderGraph& graph, cy::rendering::ResourceId frame,
                 cy::u32 frame_width, cy::u32 frame_height, cy::rendering::ResourceId target,
                 cy::u32 target_width, cy::u32 target_height) noexcept;

    /// Push constants, as shaders/present.slang's `CyRtsPresentConstants` lays them out.
    struct Constants {
        cy::u32 source[2] = {0, 0};
        cy::f32 origin[2] = {0.0F, 0.0F};
        cy::f32 size[2] = {0.0F, 0.0F};
        cy::u32 flags = 0;
        cy::u32 pad = 0;
    };
    static_assert(sizeof(Constants) == 32);

    /// The pass's recording, for the graph's callback.
    void record(const cy::rendering::PassContext& context) noexcept;

private:
    cy::rhi::Device* device_ = nullptr;
    cy::rhi::Format format_ = cy::rhi::Format::Undefined;
    cy::rhi::ShaderModuleHandle vertex_;
    cy::rhi::ShaderModuleHandle fragment_;
    cy::rhi::DescriptorSetLayoutHandle set_layout_;
    cy::rhi::PipelineLayoutHandle layout_;
    cy::rhi::GraphicsPipelineHandle pipeline_;
    cy::rendering::ResourceId frame_ = cy::rendering::kInvalidResource;
    cy::rendering::ResourceId target_ = cy::rendering::kInvalidResource;
    cy::u32 target_width_ = 0;
    cy::u32 target_height_ = 0;
    Constants constants_;
};

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_PRESENT_PASS_H
