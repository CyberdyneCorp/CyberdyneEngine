// SPDX-License-Identifier: MIT
// MSAA and multi-view through the render graph's attachment model. M11.d tasks 5.1 and 5.2.
//
// ================================================================================================
// WHY THE GRAPH OWNS THE TWIN AND THE RESOLVE
// ================================================================================================
//
// Before this, a multisampled frame was two textures and a resolve pass the FRAME declared
// (forward/frame.cpp's `colour (msaa)` and `resolve`), with a record callback the caller had to
// supply and nothing checking where it sat. A frame that read the single-sample colour before that
// resolve compiled, ran, and showed whatever the texture held — black on a fresh transient, last
// frame's pixels on an aliased one. That is the failure a derivation exists to make impossible, so
// the pass now declares a sample count and a resolve point, and the graph does the rest:
//
//   * the multisampled twin of a texture is created the first time a multisampled pass names the
//     texture as an attachment, and every attachment use of that pass lands on the twin;
//   * the resolve is a graph-owned pass inserted directly after the pass that declared it, recorded
//     by `record_resolve` below as an empty dynamic-rendering scope whose colour attachment carries
//     a resolve view — the resolve happens at the end of the scope, in the colour-output stage,
//     which is exactly what the resolve pass's declared `ColorAttachmentWrite` derives a barrier
//     for;
//   * `validate_multisampling` refuses a graph in which anything touches the texture while its twin
//     holds rendering no resolve has reached.
//
// A pass that never calls `multisample` takes none of these branches: `attachment_target` is the
// identity, no twin is created, no pass is inserted and the validation walk finds nothing to check.
// That is what "1x is byte-identical to before" rests on, and `unit.render_graph` asserts it on the
// plan hash rather than on a reading of this comment.
//
// ================================================================================================
// WHY MULTI-VIEW CHANGES RECORDING AND NOT THE PLAN
// ================================================================================================
//
// A pass that renders N views declares its attachments once, whole. With `Capability::Multiview`
// the executor records it once with a view mask; without it, once per view into a single-layer
// view of each attachment. Both touch the same subresources in the same order, so the barriers are
// the same and the plan hash is the same — the capability decides how commands are recorded and
// nothing about what the graph derived. The executor holds that branch (executor.cpp); this file
// only records the declaration and checks the attachments have a layer for every view.

#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>

#include <cy/backends/rhi/command_buffer.h>

namespace cy::rendering {
namespace {

[[nodiscard]] bool is_attachment_access(rhi::Access access) noexcept {
    switch (access) {
        case rhi::Access::ColorAttachmentWrite:
        case rhi::Access::ColorAttachmentReadWrite:
        case rhi::Access::DepthStencilAttachmentWrite:
        case rhi::Access::DepthStencilAttachmentRead:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool valid_sample_count(u32 samples) noexcept {
    return samples == 1 || samples == 2 || samples == 4 || samples == 8;
}

/// The graph-owned resolve pass's recording. An empty rendering scope: the twin is loaded, nothing
/// is drawn, and the scope's end resolves it into the target. `user` is the graph, which is stable
/// for the frame because a RenderGraph can be neither moved nor copied.
void record_resolve(const PassContext& context, void* user) noexcept {
    const auto* graph = static_cast<const RenderGraph*>(user);
    const ResourceId source = graph->pass_resolve_source(context.pass);
    const ResourceId target = graph->pass_resolve_target(context.pass);
    const TextureRequest& extent = graph->resource(target).texture;

    rhi::RenderAttachment attachment;
    attachment.view = context.executor->view(source);
    attachment.load = rhi::LoadOp::Load;
    // Stored, because a later multisampled pass may keep rendering into the same twin and resolve
    // again — the placement rule below allows exactly that.
    attachment.store = rhi::StoreOp::Store;
    attachment.resolve_view = context.executor->view(target);

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, extent.width, extent.height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&attachment, 1);
    // Every layer, so a multi-view target's twin resolves whole rather than its first view only.
    info.layer_count = extent.array_layers;
    context.commands->begin_rendering(info);
    context.commands->end_rendering();
}

}  // namespace

void RenderGraph::set_samples(PassId pass, u16 samples) noexcept {
    if (pass == kInvalidPass || pass >= passes_.size()) {
        set_failure(ErrorCode::InvalidArgument, "multisample() on no pass");
        return;
    }
    if (!valid_sample_count(samples)) {
        set_failure(ErrorCode::InvalidArgument, "multisample(): the count must be 1, 2, 4 or 8");
        return;
    }
    Pass& stored = passes_[pass];
    if (samples > 1 && stored.single_sample_reason != nullptr) {
        // THE PASS'S OWN REASON, verbatim. A pass that cannot take MSAA says why when it is
        // declared, so the refusal names the constraint rather than the call that tripped it.
        set_failure(ErrorCode::Unsupported, stored.single_sample_reason);
        return;
    }
    for (usize index = 0; index < stored.use_count; ++index) {
        if (is_attachment_access(uses_[stored.first_use + index].access)) {
            set_failure(ErrorCode::InvalidArgument,
                        "multisample() must precede the pass's attachment uses: each attachment is "
                        "redirected to its multisampled twin as it is declared");
            return;
        }
    }
    stored.samples = samples;
}

void RenderGraph::set_single_sample(PassId pass, const char* reason) noexcept {
    if (pass == kInvalidPass || pass >= passes_.size()) {
        set_failure(ErrorCode::InvalidArgument, "single_sample() on no pass");
        return;
    }
    Pass& stored = passes_[pass];
    stored.single_sample_reason =
        reason != nullptr ? reason : "this pass cannot render multisampled";
    if (stored.samples > 1) {
        set_failure(ErrorCode::Unsupported, stored.single_sample_reason);
    }
}

void RenderGraph::set_views(PassId pass, u32 views) noexcept {
    if (pass == kInvalidPass || pass >= passes_.size()) {
        set_failure(ErrorCode::InvalidArgument, "views() on no pass");
        return;
    }
    // Thirty-two is the width of a view mask. Zero views is no pass at all.
    if (views == 0 || views > 32) {
        set_failure(ErrorCode::InvalidArgument, "views(): the count must be 1 to 32");
        return;
    }
    passes_[pass].views = views;
}

void RenderGraph::request_resolve(PassId pass, ResourceId target) noexcept {
    if (pass == kInvalidPass || pass >= passes_.size()) {
        set_failure(ErrorCode::InvalidArgument, "resolve() on no pass");
        return;
    }
    if (target == kInvalidResource || target >= resources_.size() ||
        !resources_[target].is_texture) {
        set_failure(ErrorCode::InvalidArgument, "resolve() names no texture the graph has");
        return;
    }
    if (rhi::format_is_depth_stencil(resources_[target].texture.format)) {
        set_failure(ErrorCode::Unsupported,
                    "resolve(): the graph resolves colour only — the RHI exposes no depth resolve "
                    "mode, and a depth resolve is refused rather than guessed");
        return;
    }
    if (Status pushed = pending_resolves_.push_back(PendingResolve{pass, target}); !pushed) {
        set_failure(ErrorCode::OutOfMemory, "the render graph could not record a resolve");
    }
}

ResourceId RenderGraph::attachment_target(PassId pass, ResourceId resource,
                                          rhi::Access access) noexcept {
    const u16 samples = passes_[pass].samples;
    if (samples <= 1 || !is_attachment_access(access)) {
        return resource;
    }
    const ResourceInfo& info = resources_[resource];
    if (!info.is_texture || info.texture.sample_count > 1) {
        // Already multisampled: an author-managed target, rendered into as it is.
        return resource;
    }
    if (info.multisampled != kInvalidResource) {
        if (resources_[info.multisampled].texture.sample_count != samples) {
            set_failure(ErrorCode::InvalidArgument,
                        "a texture was rendered at two different sample counts in one frame; it "
                        "has one multisampled twin");
            return kInvalidResource;
        }
        return info.multisampled;
    }

    // The twin: the same texture at `samples`, graph-owned and transient, so it is aliased and
    // culled like any other. Usage is left to the declarations, as it is for every transient —
    // which keeps storage usage off a multisampled image that nothing stores into.
    TextureRequest request = info.texture;
    request.sample_count = samples;
    request.extra_usage = rhi::TextureUsage::None;
    ResourceInfo twin;
    twin.name = info.name;
    twin.is_texture = true;
    twin.transient = true;
    twin.texture = request;
    twin.resolves_into = resource;
    if (Status pushed = resources_.push_back(twin); !pushed) {
        set_failure(ErrorCode::OutOfMemory,
                    "the render graph could not record a multisampled twin");
        return kInvalidResource;
    }
    const auto twin_id = static_cast<ResourceId>(resources_.size() - 1);
    resources_[resource].multisampled = twin_id;
    return twin_id;
}

void RenderGraph::flush_resolves() noexcept {
    // Copied out first: inserting a pass must not re-enter this through add_pass().
    const usize count = pending_resolves_.size();
    for (usize index = 0; index < count && status_; ++index) {
        const PendingResolve pending = pending_resolves_[index];
        const ResourceId twin = resources_[pending.target].multisampled;
        bool wrote_twin = false;
        const Pass& declaring = passes_[pending.pass];
        for (usize use = 0; use < declaring.use_count; ++use) {
            const Use& declared = uses_[declaring.first_use + use];
            wrote_twin = wrote_twin || (declared.resource == twin && twin != kInvalidResource &&
                                        rhi::is_write(declared.access));
        }
        if (!wrote_twin) {
            set_failure(ErrorCode::InvalidArgument,
                        "resolve() was declared on a pass that renders no multisampled attachment "
                        "of that texture — declare it on the multisampled pass that writes it");
            break;
        }

        Pass pass;
        pass.name = "msaa resolve";
        pass.queue = rhi::QueueKind::Graphics;
        pass.first_use = uses_.size();
        pass.record = &record_resolve;
        pass.user = this;
        pass.resolve_source = twin;
        pass.resolve_target = pending.target;
        if (Status pushed = passes_.push_back(pass); !pushed) {
            set_failure(ErrorCode::OutOfMemory, "the render graph could not record a resolve pass");
            break;
        }
        const auto resolve_pass = static_cast<PassId>(passes_.size() - 1);
        // The twin is LOADED by the resolve scope, and the target is written in the colour-output
        // stage — which is where a Vulkan resolve happens and what the table derives for this row.
        add_use(resolve_pass, twin, rhi::Access::ColorAttachmentReadWrite, {});
        add_use(resolve_pass, pending.target, rhi::Access::ColorAttachmentWrite, {});
    }
    pending_resolves_.clear();
}

Status RenderGraph::validate_multisampling() const noexcept {
    // One flag per resource: "this twin holds rendering no resolve has reached yet". Walked in
    // declaration order, which is the topological order the scheduler already asserts.
    Array<u8> unresolved(*allocator_);
    if (Status sized = unresolved.resize(resources_.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < resources_.size(); ++index) {
        unresolved[index] = 0;
    }
    for (const Pass& pass : passes_) {
        const Span<const Use> uses(uses_.data() + pass.first_use, pass.use_count);
        if (pass.resolve_source != kInvalidResource) {
            unresolved[pass.resolve_source] = 0;
            continue;
        }
        for (const Use& use : uses) {
            const ResourceInfo& info = resources_[use.resource];
            if (info.multisampled != kInvalidResource && unresolved[info.multisampled] != 0) {
                return fail(ErrorCode::InvalidArgument,
                            "a pass uses a texture while its multisampled twin holds rendering no "
                            "resolve has reached: the resolve is missing, or declared on a pass "
                            "before the last multisampled write");
            }
            if (info.resolves_into != kInvalidResource && rhi::is_write(use.access)) {
                unresolved[use.resource] = 1;
            }
            if (pass.views > 1 && is_attachment_access(use.access) &&
                static_cast<u32>(use.range.base_layer) + pass.views > info.texture.array_layers) {
                return fail(ErrorCode::OutOfRange,
                            "a multi-view pass renders into an attachment with fewer layers than "
                            "it has views");
            }
        }
    }
    return ok();
}

u16 RenderGraph::pass_sample_count(PassId pass) const noexcept {
    return pass < passes_.size() ? passes_[pass].samples : static_cast<u16>(1);
}

u32 RenderGraph::pass_view_count(PassId pass) const noexcept {
    return pass < passes_.size() ? passes_[pass].views : 1U;
}

ResourceId RenderGraph::pass_resolve_source(PassId pass) const noexcept {
    return pass < passes_.size() ? passes_[pass].resolve_source : kInvalidResource;
}

ResourceId RenderGraph::pass_resolve_target(PassId pass) const noexcept {
    return pass < passes_.size() ? passes_[pass].resolve_target : kInvalidResource;
}

}  // namespace cy::rendering
