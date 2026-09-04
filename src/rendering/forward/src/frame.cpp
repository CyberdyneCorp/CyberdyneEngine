#include <cy/rendering/forward/frame.h>

namespace cy::rendering {

/// Everything the pass declarations read, gathered once so that each stage's function is about that
/// stage rather than about finding its inputs.
struct ForwardFrame::BuildState {
    const FrameDescription* description = nullptr;
    FrameResources* resources = nullptr;
    PrepassMode mode = PrepassMode::DepthOnly;
    bool multisampled = false;
    /// The colour attachment the shading passes write: the multisampled one where MSAA is on.
    ResourceId shading_target = kInvalidResource;
    /// The most recently written colour, threaded through the post chain.
    ResourceId current_color = kInvalidResource;
};

namespace {

using rhi::Access;
using rhi::QueueKind;
using FrameState = ForwardFrame::BuildState;

[[nodiscard]] bool valid(ResourceId resource) noexcept {
    return resource != kInvalidResource;
}

/// Attach a caller's record callback, if it supplied one for this stage.
void attach(PassBuilder& builder, const FrameDescription& description,
            FramePassKind kind) noexcept {
    const FramePassCallback& callback = description.callbacks[static_cast<u32>(kind)];
    if (callback.record != nullptr) {
        builder.record(callback.record, callback.user);
    }
}

/// The per-frame buffers and every texture the feature set calls for. Anything a feature left out
/// stays `kInvalidResource`, which is how "their targets unallocated" is observable.
void declare_textures(RenderGraph& graph, FrameState& state) noexcept {
    const FrameDescription& description = *state.description;
    FrameResources& resources = *state.resources;

    TextureRequest request;
    request.width = description.width;
    request.height = description.height;

    request.name = "depth";
    request.format = description.depth_format;
    resources.depth = graph.create_texture(request);
    if (state.multisampled) {
        request.name = "depth (msaa)";
        request.sample_count = static_cast<u16>(description.features.msaa_samples);
        resources.depth_multisampled = graph.create_texture(request);
        request.sample_count = 1;
    }

    request.name = "colour";
    request.format = description.color_format;
    resources.color = graph.create_texture(request);
    if (state.multisampled) {
        request.name = "colour (msaa)";
        request.sample_count = static_cast<u16>(description.features.msaa_samples);
        resources.color_multisampled = graph.create_texture(request);
        request.sample_count = 1;
    }

    if (state.mode != PrepassMode::DepthOnly) {
        request.name = "normal + roughness";
        request.format = description.normal_format;
        resources.normal_roughness = graph.create_texture(request);
    }
    if (state.mode == PrepassMode::DepthNormalVelocity) {
        request.name = "velocity";
        request.format = description.velocity_format;
        resources.velocity = graph.create_texture(request);
    }
    if (description.features.ambient_occlusion) {
        request.name = "ambient occlusion";
        request.format = rhi::Format::R8Unorm;
        request.extra_usage = rhi::TextureUsage::Storage;
        resources.ambient_occlusion = graph.create_texture(request);
        request.extra_usage = rhi::TextureUsage::None;
    }
    if (description.features.screen_space_gi) {
        request.name = "screen-space gi";
        request.format = description.color_format;
        request.extra_usage = rhi::TextureUsage::Storage;
        resources.screen_space_gi = graph.create_texture(request);
        request.extra_usage = rhi::TextureUsage::None;
    }
    if (description.features.screen_space_reflections) {
        request.name = "reflections";
        request.format = description.color_format;
        request.extra_usage = rhi::TextureUsage::Storage;
        resources.reflections = graph.create_texture(request);
        request.extra_usage = rhi::TextureUsage::None;
    }
    if (description.features.transparent_refraction) {
        request.name = "opaque colour copy";
        request.format = description.color_format;
        resources.opaque_color_copy = graph.create_texture(request);
    }
    if (description.features.temporal) {
        request.name = "temporal";
        request.format = description.color_format;
        request.extra_usage = rhi::TextureUsage::Storage;
        resources.temporal_history = graph.create_texture(request);
        request.extra_usage = rhi::TextureUsage::None;
    }

    // The output: the caller's imported swapchain image, or one the frame owns. A frame-owned
    // output is what a headless test and an offscreen capture get, and nothing downstream can tell
    // the difference.
    if (valid(description.output)) {
        resources.output = description.output;
    } else {
        request.name = "output";
        request.format = description.color_format;
        request.extra_usage = rhi::TextureUsage::TransferSource;
        resources.output = graph.create_texture(request);
    }
}

void declare_buffers(RenderGraph& graph, FrameState& state) noexcept {
    const FrameDescription& description = *state.description;
    FrameResources& resources = *state.resources;

    if (description.light_count > 0) {
        BufferRequest lights;
        lights.name = "lights";
        lights.size = static_cast<u64>(description.light_count) * 64U;  // sizeof(GpuLight)
        resources.lights = graph.create_buffer(lights);
    }
    const u32 clusters = description.cluster_grid.cluster_count();
    if (clusters > 0) {
        BufferRequest headers;
        headers.name = "cluster headers";
        headers.size = static_cast<u64>(clusters) * kClusterElementTypeCount * 2U * sizeof(u32);
        resources.cluster_headers = graph.create_buffer(headers);

        BufferRequest indices;
        indices.name = "cluster indices";
        indices.size = static_cast<u64>(clusters) *
                       description.cluster_grid.max_elements_per_cluster * sizeof(u32);
        resources.cluster_indices = graph.create_buffer(indices);
    }
    if (description.draw_instance_count > 0) {
        BufferRequest instances;
        instances.name = "draw instances";
        instances.size = static_cast<u64>(description.draw_instance_count) * 32U;
        resources.draw_instances = graph.create_buffer(instances);
    }
}

// --- The stages, one function each ---------------------------------------------------------------

/// Stage 1. Uploads the per-frame buffers. A transfer, and the only pass that writes them.
PassId declare_prepare(RenderGraph& graph, FrameState& state) noexcept {
    const FrameResources& resources = *state.resources;
    if (!valid(resources.lights) && !valid(resources.draw_instances)) {
        return kInvalidPass;
    }
    PassBuilder builder = graph.add_pass("prepare", QueueKind::Graphics);
    if (valid(resources.lights)) {
        builder.write(resources.lights, Access::TransferWrite);
    }
    if (valid(resources.draw_instances)) {
        builder.write(resources.draw_instances, Access::TransferWrite);
    }
    attach(builder, *state.description, FramePassKind::Prepare);
    return builder.id();
}

/// Stage 2. Depth, and whatever the derived mode adds to it.
PassId declare_prepass(RenderGraph& graph, FrameState& state) noexcept {
    const FrameResources& resources = *state.resources;
    const ResourceId depth = state.multisampled ? resources.depth_multisampled : resources.depth;
    PassBuilder builder = graph.add_pass("depth prepass", QueueKind::Graphics);
    builder.write(depth, Access::DepthStencilAttachmentWrite);
    if (valid(resources.draw_instances)) {
        builder.read(resources.draw_instances, Access::VertexStorageRead);
    }
    if (valid(resources.normal_roughness)) {
        builder.write(resources.normal_roughness, Access::ColorAttachmentWrite);
    }
    if (valid(resources.velocity)) {
        builder.write(resources.velocity, Access::ColorAttachmentWrite);
    }
    attach(builder, *state.description, FramePassKind::DepthPrepass);
    return builder.id();
}

/// Stage 2b. The MSAA depth resolve, so the screen-space passes work at single-sample resolution.
PassId declare_depth_resolve(RenderGraph& graph, FrameState& state) noexcept {
    const FrameResources& resources = *state.resources;
    PassBuilder builder = graph.add_pass("depth resolve", QueueKind::Graphics);
    builder.read(resources.depth_multisampled, Access::FragmentSampledRead);
    builder.write(resources.depth, Access::DepthStencilAttachmentWrite);
    attach(builder, *state.description, FramePassKind::DepthResolve);
    return builder.id();
}

/// Stage 3. The compute pass `rendering-forward-clustered` requires, on the async queue where the
/// device has one. The graph folds it onto graphics where it does not, from these declarations.
PassId declare_cluster_assignment(RenderGraph& graph, FrameState& state) noexcept {
    const FrameResources& resources = *state.resources;
    if (!valid(resources.cluster_headers)) {
        return kInvalidPass;
    }
    PassBuilder builder = graph.add_pass("cluster assignment", state.description->cluster_queue);
    if (valid(resources.lights)) {
        builder.read(resources.lights, Access::ComputeStorageRead);
    }
    builder.write(resources.cluster_headers, Access::ComputeStorageWrite);
    builder.write(resources.cluster_indices, Access::ComputeStorageWrite);
    attach(builder, *state.description, FramePassKind::ClusterAssignment);
    return builder.id();
}

/// Stages 4. The screen-space passes that need depth and normals. One function, because they read
/// the same two inputs and differ only in what they write.
PassId declare_screen_space(RenderGraph& graph, FrameState& state, const char* name,
                            ResourceId target, FramePassKind kind) noexcept {
    const FrameResources& resources = *state.resources;
    PassBuilder builder = graph.add_pass(name, QueueKind::Graphics);
    builder.read(resources.depth, Access::ComputeSampledRead);
    if (valid(resources.normal_roughness)) {
        builder.read(resources.normal_roughness, Access::ComputeSampledRead);
    }
    builder.write(target, Access::ComputeStorageWrite);
    attach(builder, *state.description, kind);
    return builder.id();
}

/// Stage 5. Clustered forward shading into HDR colour.
///
/// The depth declaration is the interesting one: with a prepass it is a READ, because the opaque
/// pass tests `Equal` and writes nothing. Without one it is a write. See the header comment.
PassId declare_opaque(RenderGraph& graph, FrameState& state) noexcept {
    const FrameDescription& description = *state.description;
    const FrameResources& resources = *state.resources;
    const ResourceId depth = state.multisampled ? resources.depth_multisampled : resources.depth;

    PassBuilder builder = graph.add_pass("opaque", QueueKind::Graphics);
    builder.write(state.shading_target, Access::ColorAttachmentWrite);
    builder.use(depth, description.features.depth_prepass ? Access::DepthStencilAttachmentRead
                                                          : Access::DepthStencilAttachmentWrite);
    if (valid(resources.draw_instances)) {
        builder.read(resources.draw_instances, Access::VertexStorageRead);
    }
    if (valid(resources.cluster_headers)) {
        builder.read(resources.cluster_headers, Access::FragmentStorageRead);
        builder.read(resources.cluster_indices, Access::FragmentStorageRead);
    }
    if (valid(resources.lights)) {
        builder.read(resources.lights, Access::FragmentStorageRead);
    }
    if (valid(resources.ambient_occlusion)) {
        builder.read(resources.ambient_occlusion, Access::FragmentSampledRead);
    }
    if (valid(resources.screen_space_gi)) {
        builder.read(resources.screen_space_gi, Access::FragmentSampledRead);
    }
    attach(builder, description, FramePassKind::Opaque);
    return builder.id();
}

/// Stage 6.
PassId declare_sky(RenderGraph& graph, FrameState& state) noexcept {
    const FrameResources& resources = *state.resources;
    const ResourceId depth = state.multisampled ? resources.depth_multisampled : resources.depth;
    PassBuilder builder = graph.add_pass("sky", QueueKind::Graphics);
    builder.use(state.shading_target, Access::ColorAttachmentReadWrite);
    builder.use(depth, Access::DepthStencilAttachmentRead);
    attach(builder, *state.description, FramePassKind::Sky);
    return builder.id();
}

/// Stage 8's prerequisite: the copy of the opaque result a refracting material samples.
PassId declare_opaque_copy(RenderGraph& graph, FrameState& state) noexcept {
    const FrameResources& resources = *state.resources;
    PassBuilder builder = graph.add_pass("opaque colour copy", QueueKind::Graphics);
    builder.read(state.shading_target, Access::TransferRead);
    builder.write(resources.opaque_color_copy, Access::TransferWrite);
    attach(builder, *state.description, FramePassKind::OpaqueColorCopy);
    return builder.id();
}

/// Stage 8.
PassId declare_transparent(RenderGraph& graph, FrameState& state) noexcept {
    const FrameResources& resources = *state.resources;
    const ResourceId depth = state.multisampled ? resources.depth_multisampled : resources.depth;
    PassBuilder builder = graph.add_pass("transparent", QueueKind::Graphics);
    builder.use(state.shading_target, Access::ColorAttachmentReadWrite);
    builder.use(depth, Access::DepthStencilAttachmentRead);
    if (valid(resources.cluster_headers)) {
        builder.read(resources.cluster_headers, Access::FragmentStorageRead);
        builder.read(resources.cluster_indices, Access::FragmentStorageRead);
    }
    if (valid(resources.lights)) {
        builder.read(resources.lights, Access::FragmentStorageRead);
    }
    if (valid(resources.opaque_color_copy)) {
        builder.read(resources.opaque_color_copy, Access::FragmentSampledRead);
    }
    if (valid(resources.reflections)) {
        builder.read(resources.reflections, Access::FragmentSampledRead);
    }
    attach(builder, *state.description, FramePassKind::Transparent);
    return builder.id();
}

}  // namespace

const char* prepass_mode_name(PrepassMode mode) noexcept {
    switch (mode) {
        case PrepassMode::DepthOnly:
            return "depth-only";
        case PrepassMode::DepthNormal:
            return "depth-normal";
        case PrepassMode::DepthNormalVelocity:
            return "depth-normal-velocity";
        case PrepassMode::Count:
            break;
    }
    return "unknown";
}

PrepassMode select_prepass_mode(const FrameFeatures& features) noexcept {
    // Motion vectors first: anything temporal needs them, and so does motion blur.
    if (features.temporal || features.motion_blur) {
        return PrepassMode::DepthNormalVelocity;
    }
    if (features.ambient_occlusion || features.screen_space_gi ||
        features.screen_space_reflections) {
        return PrepassMode::DepthNormal;
    }
    return PrepassMode::DepthOnly;
}

const char* frame_pass_kind_name(FramePassKind kind) noexcept {
    switch (kind) {
        case FramePassKind::Prepare:
            return "prepare";
        case FramePassKind::DepthPrepass:
            return "depth prepass";
        case FramePassKind::DepthResolve:
            return "depth resolve";
        case FramePassKind::ClusterAssignment:
            return "cluster assignment";
        case FramePassKind::AmbientOcclusion:
            return "ambient occlusion";
        case FramePassKind::ScreenSpaceGi:
            return "screen-space gi";
        case FramePassKind::Opaque:
            return "opaque";
        case FramePassKind::Sky:
            return "sky";
        case FramePassKind::ScreenSpaceReflections:
            return "screen-space reflections";
        case FramePassKind::OpaqueColorCopy:
            return "opaque colour copy";
        case FramePassKind::Transparent:
            return "transparent";
        case FramePassKind::Resolve:
            return "resolve";
        case FramePassKind::Temporal:
            return "temporal";
        case FramePassKind::PostProcess:
            return "post-process";
        case FramePassKind::UiAndDebug:
            return "ui and debug";
        case FramePassKind::Composite:
            return "composite";
        case FramePassKind::Present:
            return "present";
        case FramePassKind::Count:
            break;
    }
    return "unknown";
}

ForwardFrame::ForwardFrame(Allocator& allocator) noexcept : passes_(allocator) {}

PassId ForwardFrame::pass_of(FramePassKind kind) const noexcept {
    for (const FramePass& pass : passes_.span()) {
        if (pass.kind == kind) {
            return pass.pass;
        }
    }
    return kInvalidPass;
}

void ForwardFrame::stage(FramePassKind kind, const char* name, PassId pass) noexcept {
    if (pass == kInvalidPass || !status_) {
        return;
    }
    status_ = passes_.push_back(FramePass{kind, name, pass});
}

Status ForwardFrame::declare_resources(RenderGraph& graph,
                                       const FrameDescription& description) noexcept {
    BuildState state;
    state.description = &description;
    state.resources = &resources_;
    state.mode = prepass_mode_;
    state.multisampled = description.features.msaa_samples > 1;
    declare_textures(graph, state);
    declare_buffers(graph, state);
    return graph.status();
}

void ForwardFrame::declare_prepare_and_depth(RenderGraph& graph, BuildState& state) noexcept {
    const FrameFeatures& features = state.description->features;

    // 1. Prepare.
    stage(FramePassKind::Prepare, "prepare", declare_prepare(graph, state));

    // 2. Depth prepass, and its MSAA resolve — "plus MSAA depth resolve if required", so that the
    // screen-space passes below work at single-sample resolution.
    if (features.depth_prepass) {
        stage(FramePassKind::DepthPrepass, "depth prepass", declare_prepass(graph, state));
        if (state.multisampled) {
            stage(FramePassKind::DepthResolve, "depth resolve",
                  declare_depth_resolve(graph, state));
        }
    }

    // 3. Cluster assignment.
    stage(FramePassKind::ClusterAssignment, "cluster assignment",
          declare_cluster_assignment(graph, state));

    // 4. The screen-space passes that need depth and normals.
    if (features.ambient_occlusion) {
        stage(FramePassKind::AmbientOcclusion, "ambient occlusion",
              declare_screen_space(graph, state, "ambient occlusion", resources_.ambient_occlusion,
                                   FramePassKind::AmbientOcclusion));
    }
    if (features.screen_space_gi) {
        stage(FramePassKind::ScreenSpaceGi, "screen-space gi",
              declare_screen_space(graph, state, "screen-space gi", resources_.screen_space_gi,
                                   FramePassKind::ScreenSpaceGi));
    }
}

void ForwardFrame::declare_shading(RenderGraph& graph, BuildState& state) noexcept {
    const FrameFeatures& features = state.description->features;

    // 5, 6.
    stage(FramePassKind::Opaque, "opaque", declare_opaque(graph, state));
    if (features.sky) {
        stage(FramePassKind::Sky, "sky", declare_sky(graph, state));
    }
    // 7.
    if (features.screen_space_reflections) {
        stage(FramePassKind::ScreenSpaceReflections, "screen-space reflections",
              declare_screen_space(graph, state, "screen-space reflections", resources_.reflections,
                                   FramePassKind::ScreenSpaceReflections));
    }
    // 8, with its refraction copy first — the copy is a declared pass and the read is a declared
    // read, which is what makes "the graph SHALL synchronise it" true without anybody saying so.
    if (features.transparency) {
        if (features.transparent_refraction) {
            stage(FramePassKind::OpaqueColorCopy, "opaque colour copy",
                  declare_opaque_copy(graph, state));
        }
        stage(FramePassKind::Transparent, "transparent", declare_transparent(graph, state));
    }
    state.current_color = state.shading_target;
}

void ForwardFrame::declare_post_chain(RenderGraph& graph, BuildState& state) noexcept {
    const FrameDescription& description = *state.description;
    const FrameFeatures& features = description.features;

    // 9. The MSAA colour resolve.
    if (state.multisampled) {
        PassBuilder builder = graph.add_pass("resolve", QueueKind::Graphics);
        builder.read(resources_.color_multisampled, Access::FragmentSampledRead);
        builder.write(resources_.color, Access::ColorAttachmentWrite);
        attach(builder, description, FramePassKind::Resolve);
        stage(FramePassKind::Resolve, "resolve", builder.id());
        state.current_color = resources_.color;
    }

    // 10. Temporal.
    if (features.temporal) {
        PassBuilder builder = graph.add_pass("temporal", QueueKind::Graphics);
        builder.read(state.current_color, Access::ComputeSampledRead);
        if (valid(resources_.velocity)) {
            builder.read(resources_.velocity, Access::ComputeSampledRead);
        }
        builder.read(resources_.depth, Access::ComputeSampledRead);
        builder.write(resources_.temporal_history, Access::ComputeStorageWrite);
        attach(builder, description, FramePassKind::Temporal);
        stage(FramePassKind::Temporal, "temporal", builder.id());
        state.current_color = resources_.temporal_history;
    }

    // 11. Post-process, which tonemaps STRAIGHT INTO THE OUTPUT. That is what removes the composite
    // blit in the ordinary case: the swapchain image is a colour attachment like any other.
    if (features.post_process) {
        PassBuilder builder = graph.add_pass("post-process", QueueKind::Graphics);
        builder.read(state.current_color, Access::FragmentSampledRead);
        builder.write(resources_.output, Access::ColorAttachmentWrite);
        attach(builder, description, FramePassKind::PostProcess);
        stage(FramePassKind::PostProcess, "post-process", builder.id());
        resources_.post_color = resources_.output;
        state.current_color = resources_.output;
    }

    // 12. UI and debug, drawn after tonemapping.
    if (features.ui) {
        PassBuilder builder = graph.add_pass("ui and debug", QueueKind::Graphics);
        builder.use(state.current_color, Access::ColorAttachmentReadWrite);
        attach(builder, description, FramePassKind::UiAndDebug);
        stage(FramePassKind::UiAndDebug, "ui and debug", builder.id());
    }

    // 13a. The blit, only when the chain did not already land in the output.
    if (state.current_color != resources_.output) {
        PassBuilder builder = graph.add_pass("composite", QueueKind::Graphics);
        builder.read(state.current_color, Access::TransferRead);
        builder.write(resources_.output, Access::TransferWrite);
        attach(builder, description, FramePassKind::Composite);
        stage(FramePassKind::Composite, "composite", builder.id());
    }

    // 13b. The presentation transition: recordless, side-effecting, and the only thing in the frame
    // that mentions presentation at all. The graph derives the layout transition from it.
    PassBuilder present = graph.add_pass("present", QueueKind::Graphics);
    present.read(resources_.output, Access::Present);
    present.side_effect();
    stage(FramePassKind::Present, "present", present.id());
}

Status ForwardFrame::declare_passes(RenderGraph& graph,
                                    const FrameDescription& description) noexcept {
    BuildState state;
    state.description = &description;
    state.resources = &resources_;
    state.mode = prepass_mode_;
    state.multisampled = description.features.msaa_samples > 1;
    state.shading_target = state.multisampled ? resources_.color_multisampled : resources_.color;

    declare_prepare_and_depth(graph, state);
    declare_shading(graph, state);
    declare_post_chain(graph, state);

    if (!status_) {
        return status_;
    }
    return graph.status();
}

Status ForwardFrame::build(RenderGraph& graph, const FrameDescription& description) noexcept {
    passes_.clear();
    resources_ = FrameResources{};
    status_ = ok();

    if (description.width == 0 || description.height == 0) {
        return fail(ErrorCode::InvalidArgument, "forward frame: the viewport is empty");
    }
    const u32 samples = description.features.msaa_samples;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) {
        return fail(ErrorCode::InvalidArgument, "forward frame: MSAA must be 1, 2, 4 or 8");
    }
    // EVERY SCREEN-SPACE AND TEMPORAL FEATURE READS THE PREPASS'S OUTPUT. Allowing one without a
    // prepass would declare a pass that samples a depth target nothing wrote — which compiles,
    // runs, and produces a frame that is subtly wrong rather than obviously broken. The
    // specification's own scenario ("SSAO enabled ... the prepass SHALL run in DepthNormal mode")
    // assumes the prepass is there, so this is that assumption stated as a refusal.
    const FrameFeatures& features = description.features;
    const bool needs_prepass = features.ambient_occlusion || features.screen_space_gi ||
                               features.screen_space_reflections || features.temporal ||
                               features.motion_blur;
    if (needs_prepass && !features.depth_prepass) {
        return fail(ErrorCode::InvalidArgument,
                    "forward frame: screen-space and temporal features require the depth prepass "
                    "that produces the depth, normal and velocity targets they read");
    }
    prepass_mode_ = select_prepass_mode(description.features);
    // A prepass mode that names targets nothing will fill is the failure the derivation exists to
    // prevent, so it is stated: without a prepass there are no prepass outputs, whatever the
    // features asked for.
    if (!description.features.depth_prepass) {
        prepass_mode_ = PrepassMode::DepthOnly;
    }

    if (Status declared = declare_resources(graph, description); !declared) {
        return declared;
    }
    return declare_passes(graph, description);
}

}  // namespace cy::rendering
