// SPDX-License-Identifier: MIT
// Ribbons, trails and beams, composited. M11.c task 6.3. See strip_renderer.h.

#include <cy/rendering/particles/strip_renderer.h>

#include "strip_msl.h"
#include "strip_spirv.h"

#include <cstddef>

namespace cy::rendering::particles {
namespace {

inline constexpr u32 kVerticesPerSegment = 6;

// `cy/strip.slang`'s `CyStripVertex`, field by field: a float4 of position and half-width, a float4
// of colour, and a float4 whose x is `along` and whose y is the strip's bits.
static_assert(offsetof(StripVertex, position) == 0);
static_assert(offsetof(StripVertex, half_width) == 12);
static_assert(offsetof(StripVertex, color) == 16);
static_assert(offsetof(StripVertex, along) == 32);
static_assert(offsetof(StripVertex, strip) == 36);

/// The extension's callback: the frame's sets 0 and 1 are already bound, this binds set 2 and the
/// pipeline and issues ONE draw of a quad per neighbouring pair.
void record_strips(const pipeline::ExtensionContext& context, void* user) noexcept {
    auto* renderer = static_cast<StripRenderer*>(user);
    if (renderer == nullptr || !renderer->ready() || renderer->live() < 2) {
        return;
    }
    if (!context.inside_rendering || context.commands == nullptr) {
        return;
    }
    rhi::CommandBuffer& commands = *context.commands;
    renderer->draw().bind(commands);
    commands.draw((renderer->live() - 1U) * kVerticesPerSegment, 1, 0, 0);
    ++renderer->mutable_report().draws;
}

}  // namespace

StripReport count_strips(Span<const StripVertex> vertices) noexcept {
    StripReport report;
    report.vertices = static_cast<u32>(vertices.size());
    for (usize index = 0; index < vertices.size(); ++index) {
        const bool continues = index > 0 && vertices[index].strip == vertices[index - 1].strip;
        if (continues) {
            ++report.segments;
        } else {
            ++report.strips;
        }
    }
    return report;
}

StripRenderer::~StripRenderer() {
    shutdown();
}

Status StripRenderer::initialize(rhi::Device& device, const pipeline::FramePipelines& pipelines,
                                 u32 capacity) noexcept {
    if (ready_) {
        return fail(ErrorCode::InvalidArgument, "strip renderer: already initialized");
    }
    if (capacity < 2) {
        return fail(ErrorCode::InvalidArgument,
                    "strip renderer: a strip needs two vertices, so a ring of fewer draws nothing "
                    "and hides the reason");
    }
    detail::TransparentDrawDescription description;
    description.name = "strips";
    description.vertex.name = "cy strip vertex";
    description.vertex.spirv =
        Span<const u32>(kStripVertexSpirv, sizeof(kStripVertexSpirv) / sizeof(u32));
    description.vertex.msl =
        Span<const u8>(reinterpret_cast<const u8*>(kStripVertexMsl), sizeof(kStripVertexMsl) - 1);
    description.vertex.msl_entry = "cyStripVertex";
    description.fragment.name = "cy strip fragment";
    description.fragment.spirv =
        Span<const u32>(kStripFragmentSpirv, sizeof(kStripFragmentSpirv) / sizeof(u32));
    description.fragment.msl = Span<const u8>(reinterpret_cast<const u8*>(kStripFragmentMsl),
                                              sizeof(kStripFragmentMsl) - 1);
    description.fragment.msl_entry = "cyStripFragment";
    description.record_size = sizeof(StripVertex);
    description.capacity = capacity;
    if (Status made = draw_.initialize(device, pipelines, description); !made) {
        return made;
    }
    capacity_ = capacity;
    ready_ = true;
    return ok();
}

Status StripRenderer::upload(u32 frame_slot, Span<const StripVertex> vertices) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "strip renderer: not initialized");
    }
    live_ = static_cast<u32>(vertices.size() > capacity_ ? capacity_ : vertices.size());
    const u32 draws = report_.draws;
    report_ = count_strips(Span<const StripVertex>(vertices.data(), live_));
    report_.draws = draws;
    report_.dropped = static_cast<u32>(vertices.size()) - live_;
    return draw_.upload(frame_slot, vertices.data(), live_);
}

pipeline::PassExtension StripRenderer::extension() noexcept {
    pipeline::PassExtension extension;
    extension.kind = FramePassKind::Transparent;
    extension.record = &record_strips;
    extension.user = this;
    return extension;
}

void StripRenderer::shutdown() noexcept {
    draw_.shutdown();
    capacity_ = 0;
    live_ = 0;
    ready_ = false;
}

}  // namespace cy::rendering::particles
