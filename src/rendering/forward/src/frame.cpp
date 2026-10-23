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
    /// What a produced stage is handed as its depth, motion and extent: the scene's own targets at
    /// the render extent up to the temporal stage, and the temporal producer's output-extent ones
    /// after it when it wrote them.
    ResourceId stage_depth = kInvalidResource;
    ResourceId stage_velocity = kInvalidResource;
    u32 stage_width = 0;
    u32 stage_height = 0;
};

namespace {

using rhi::Access;
using rhi::QueueKind;
using FrameState = ForwardFrame::BuildState;

/// Why the virtual-geometry stage cannot render multisampled, in one place: build() refuses a
/// multisampled frame with it and the stage's declaration hands it to the graph.
constexpr const char* kVirtualGeometrySingleSample =
    "forward frame: virtual geometry's visibility target is single-sample, and this frame is "
    "multisampled";

[[nodiscard]] bool valid(ResourceId resource) noexcept {
    return resource != kInvalidResource;
}

/// The extent the frame ends at — see `FrameDescription::output_width`. The render extent unless
/// the description names another.
[[nodiscard]] u32 output_width(const FrameDescription& description) noexcept {
    return description.output_width == 0 ? description.width : description.output_width;
}

[[nodiscard]] u32 output_height(const FrameDescription& description) noexcept {
    return description.output_height == 0 ? description.height : description.output_height;
}

/// Whether the frame's temporal stage is a producer's passes rather than the single resolve.
[[nodiscard]] bool produced_temporal(const FrameDescription& description) noexcept {
    return description.features.temporal && description.temporal_stage.declare != nullptr;
}

/// Whether anything after the temporal stage reads the scene's depth or motion, and so needs them
/// at the output extent when the producer upscales.
[[nodiscard]] bool post_reads_depth(const FrameFeatures& features) noexcept {
    return features.depth_of_field || features.motion_blur || features.selection_outlines ||
           features.fog_of_war;
}

/// The pre-translucency copy: refraction's, or a temporal producer's reactive mask's.
[[nodiscard]] bool copies_opaque_colour(const FrameFeatures& features) noexcept {
    return features.transparent_refraction || (features.reactive_mask && features.transparency);
}

/// Attach a caller's record callback, if it supplied one for this stage.
void attach(PassBuilder& builder, const FrameDescription& description,
            FramePassKind kind) noexcept {
    const FramePassCallback& callback = description.callbacks[static_cast<u32>(kind)];
    for (const ResourceId resource : callback.vertex_reads) {
        if (valid(resource)) {
            builder.read(resource, Access::VertexAttributeRead);
        }
    }
    for (const FrameResourceRead& read : callback.reads) {
        if (valid(read.resource)) {
            builder.read(read.resource, read.access);
        }
    }
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
    if (description.features.virtual_geometry) {
        // A transfer source because virtual geometry's material resolve reads the payload out of it
        // as a buffer — the same `uint2` visibility buffer its compute rasteriser fills — so the
        // classification, the bins and the resolve do not know which rasteriser ran.
        request.name = "visibility";
        request.format = description.visibility_format;
        request.extra_usage = rhi::TextureUsage::TransferSource;
        resources.visibility = graph.create_texture(request);
        request.extra_usage = rhi::TextureUsage::None;
    }
    if (description.features.ambient_occlusion) {
        if (valid(description.ambient_occlusion_target)) {
            // The producer owns the term's storage: its consumers sample it through a slot that
            // must outlive the graph, and a transient's view is recreated every frame.
            resources.ambient_occlusion = description.ambient_occlusion_target;
        } else {
            request.name = "ambient occlusion";
            request.format = rhi::Format::R8Unorm;
            request.extra_usage = rhi::TextureUsage::Storage;
            resources.ambient_occlusion = graph.create_texture(request);
            request.extra_usage = rhi::TextureUsage::None;
        }
    }
    if (description.features.contact_shadows) {
        // Always the producer's: the opaque pass samples it through a texture-table slot, and a
        // transient's view is recreated every frame.
        resources.contact_shadows = description.contact_shadows_target;
    }
    if (description.features.volumetric_fog) {
        // Always the producer's, for the contact shadows' reason: the opaque pass samples it
        // through a texture-table slot.
        resources.volumetric_fog = description.volumetric_fog_target;
    }
    if (description.features.many_lights) {
        // The producer's, for the same reason: both are sampled through texture-table slots.
        resources.many_lights = description.many_lights_target;
        resources.many_lights_reservoir = description.many_lights_reservoir_target;
    }
    if (description.features.screen_space_gi) {
        if (description.screen_space_gi_stage.declare != nullptr) {
            // The producer's, as the contact shadows' are: the opaque pass samples both targets
            // through texture-table slots, and a transient's view is recreated every frame.
            resources.screen_space_gi = description.screen_space_gi_target;
            resources.screen_space_gi_specular = description.screen_space_gi_specular_target;
        } else {
            request.name = "screen-space gi";
            request.format = description.color_format;
            request.extra_usage = rhi::TextureUsage::Storage;
            resources.screen_space_gi = graph.create_texture(request);
            request.extra_usage = rhi::TextureUsage::None;
        }
    }
    if (description.features.screen_space_reflections) {
        request.name = "reflections";
        request.format = description.color_format;
        request.extra_usage = rhi::TextureUsage::Storage;
        resources.reflections = graph.create_texture(request);
        request.extra_usage = rhi::TextureUsage::None;
    }
    if (copies_opaque_colour(description.features)) {
        request.name = "opaque colour copy";
        request.format = description.color_format;
        resources.opaque_color_copy = graph.create_texture(request);
    }
    // A producer of the temporal stage owns its history, at whatever extent it reconstructs to.
    if (description.features.temporal && !produced_temporal(description)) {
        request.name = "temporal previous";
        request.format = description.color_format;
        resources.temporal_previous = valid(description.temporal_previous)
                                          ? description.temporal_previous
                                          : graph.create_texture(request);
        request.name = "temporal current";
        resources.temporal_history = valid(description.temporal_current)
                                         ? description.temporal_current
                                         : graph.create_texture(request);
    }

    // The output: the caller's imported swapchain image, or one the frame owns. A frame-owned
    // output is what a headless test and an offscreen capture get, and nothing downstream can tell
    // the difference.
    if (valid(description.output)) {
        resources.output = description.output;
    } else {
        request.width = output_width(description);
        request.height = output_height(description);
        request.name = "output";
        request.format = description.color_format;
        request.extra_usage = rhi::TextureUsage::TransferSource;
        resources.output = graph.create_texture(request);
    }
}

void declare_buffers(RenderGraph& graph, FrameState& state) noexcept {
    const FrameDescription& description = *state.description;
    FrameResources& resources = *state.resources;

    if (description.light_count > 0 && valid(description.lights_target)) {
        resources.lights = description.lights_target;
    } else if (description.light_count > 0) {
        BufferRequest lights;
        lights.name = "lights";
        lights.size = static_cast<u64>(description.light_count) * 64U;  // sizeof(GpuLight)
        resources.lights = graph.create_buffer(lights);
    }
    const u32 clusters = description.cluster_grid.cluster_count();
    if (clusters > 0 && valid(description.cluster_headers_target)) {
        // The device assignment's: imported, because the frame's set 1 binds the producer's own
        // buffers rather than transients the graph recreates.
        resources.cluster_headers = description.cluster_headers_target;
        resources.cluster_indices = description.cluster_indices_target;
    } else if (clusters > 0) {
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

PassId declare_shadow(RenderGraph& graph, FrameState& state) noexcept {
    const FrameDescription& description = *state.description;
    if (!valid(description.shadow_color) || !valid(description.shadow_depth)) {
        return kInvalidPass;
    }
    PassBuilder builder = graph.add_pass("directional shadow", QueueKind::Graphics);
    builder.write(description.shadow_color, Access::ColorAttachmentWrite);
    builder.write(description.shadow_depth, Access::DepthStencilAttachmentWrite);
    if (valid(state.resources->draw_instances)) {
        builder.read(state.resources->draw_instances, Access::VertexStorageRead);
    }
    attach(builder, description, FramePassKind::Shadow);
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

/// Stage 2c. Virtual geometry's visibility pass — see `FramePassKind::VirtualGeometry` for why it
/// sits here. It WRITES depth rather than reading it: the hardware depth test is how two clusters,
/// or a cluster and a mesh the prepass drew, decide which one a pixel shows.
PassId declare_virtual_geometry(RenderGraph& graph, FrameState& state) noexcept {
    const FrameResources& resources = *state.resources;
    PassBuilder builder = graph.add_pass("virtual geometry", QueueKind::Graphics);
    // Stated to the graph as well as refused by build(): a pass that later asks this stage for a
    // sample count gets the same reason from the graph, not a twin nothing can resolve.
    builder.single_sample(kVirtualGeometrySingleSample);
    builder.write(resources.visibility, Access::ColorAttachmentWrite);
    builder.write(resources.depth, Access::DepthStencilAttachmentWrite);
    attach(builder, *state.description, FramePassKind::VirtualGeometry);
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
    if (valid(resources.contact_shadows)) {
        builder.read(resources.contact_shadows, Access::FragmentSampledRead);
    }
    if (valid(resources.screen_space_gi)) {
        builder.read(resources.screen_space_gi, Access::FragmentSampledRead);
    }
    if (valid(resources.screen_space_gi_specular)) {
        builder.read(resources.screen_space_gi_specular, Access::FragmentSampledRead);
    }
    if (valid(resources.many_lights)) {
        builder.read(resources.many_lights, Access::FragmentSampledRead);
        builder.read(resources.many_lights_reservoir, Access::FragmentSampledRead);
    }
    if (valid(resources.volumetric_fog)) {
        // A texture the opaque pass samples through the frame's table, or — for a caller that
        // composites the fog into the atmosphere's own froxel table — a buffer it reads as one.
        builder.read(resources.volumetric_fog, graph.resource(resources.volumetric_fog).is_texture
                                                   ? Access::FragmentSampledRead
                                                   : Access::FragmentStorageRead);
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
    // Refraction samples the copy; a copy made only for a temporal producer's reactive mask is not
    // the transparent pass's to read.
    if (valid(resources.opaque_color_copy) && state.description->features.transparent_refraction) {
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
    if (features.ambient_occlusion || features.contact_shadows || features.screen_space_gi ||
        features.screen_space_reflections || features.normal_prepass || features.many_lights) {
        return PrepassMode::DepthNormal;
    }
    return PrepassMode::DepthOnly;
}

const char* frame_pass_kind_name(FramePassKind kind) noexcept {
    switch (kind) {
        case FramePassKind::Prepare:
            return "prepare";
        case FramePassKind::Shadow:
            return "shadow";
        case FramePassKind::DepthPrepass:
            return "depth prepass";
        case FramePassKind::DepthResolve:
            return "depth resolve";
        case FramePassKind::VirtualGeometry:
            return "virtual geometry";
        case FramePassKind::ClusterAssignment:
            return "cluster assignment";
        case FramePassKind::AmbientOcclusion:
            return "ambient occlusion";
        case FramePassKind::ContactShadows:
            return "contact shadows";
        case FramePassKind::ScreenSpaceGi:
            return "screen-space gi";
        case FramePassKind::VolumetricFog:
            return "volumetric fog";
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
        case FramePassKind::DepthOfField:
            return "depth of field";
        case FramePassKind::MotionBlur:
            return "motion blur";
        case FramePassKind::Bloom:
            return "bloom";
        case FramePassKind::PostProcess:
            return "post-process";
        case FramePassKind::SelectionOutlines:
            return "selection outlines";
        case FramePassKind::UiAndDebug:
            return "ui and debug";
        case FramePassKind::Composite:
            return "composite";
        case FramePassKind::Present:
            return "present";
        case FramePassKind::FogOfWar:
            return "fog of war";
        case FramePassKind::ManyLights:
            return "many lights";
        case FramePassKind::UiHdr:
            return "ui hdr";
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

void ForwardFrame::declare_produced_stage(RenderGraph& graph, const BuildState& state,
                                          const FrameStageDeclaration& producer, FramePassKind kind,
                                          const char* name, ResourceId target,
                                          ResourceId source) noexcept {
    if (!status_) {
        return;
    }
    ScreenSpaceStageInputs inputs;
    inputs.depth = state.stage_depth;
    inputs.normal_roughness = resources_.normal_roughness;
    inputs.target = target;
    inputs.draw_instances = resources_.draw_instances;
    inputs.source = source;
    inputs.velocity = state.stage_velocity;
    inputs.color = state.current_color;
    inputs.width = state.stage_width;
    inputs.height = state.stage_height;
    inputs.render_width = state.description->width;
    inputs.render_height = state.description->height;
    inputs.lights = resources_.lights;
    const PassId first = producer.declare(graph, inputs, producer.user);
    if (first == kInvalidPass) {
        // A producer that refused is a frame that cannot shade what it was configured to shade;
        // leaving the stage out would draw the frame without the term and say nothing.
        status_ = fail(ErrorCode::InvalidArgument,
                       "forward frame: a stage's producer refused to declare its passes");
        return;
    }
    stage(kind, name, first);
}

void ForwardFrame::declare_temporal_producer(RenderGraph& graph, BuildState& state) noexcept {
    const FrameDescription& description = *state.description;
    const u32 width = output_width(description);
    const u32 height = output_height(description);
    TextureRequest request;
    request.width = width;
    request.height = height;
    request.name = "upscaled colour";
    request.format = description.color_format;
    request.extra_usage = rhi::TextureUsage::Storage;
    resources_.upscaled = graph.create_texture(request);
    request.extra_usage = rhi::TextureUsage::None;
    // The depth and motion the stages after this one read, at the extent they run at — only when
    // one of them reads them, and only when that extent is not the scene's own.
    const bool rescaled = width != description.width || height != description.height;
    if (rescaled && post_reads_depth(description.features)) {
        request.name = "output depth";
        request.format = description.depth_format;
        resources_.output_depth = graph.create_texture(request);
        request.name = "output velocity";
        request.format = description.velocity_format;
        resources_.output_velocity = graph.create_texture(request);
    }

    ScreenSpaceStageInputs inputs;
    inputs.depth = resources_.depth;
    inputs.normal_roughness = resources_.normal_roughness;
    inputs.target = resources_.upscaled;
    inputs.draw_instances = resources_.draw_instances;
    inputs.source = state.current_color;
    inputs.velocity = resources_.velocity;
    inputs.color = state.current_color;
    inputs.width = width;
    inputs.height = height;
    inputs.render_width = description.width;
    inputs.render_height = description.height;
    inputs.opaque_color =
        description.features.reactive_mask ? resources_.opaque_color_copy : kInvalidResource;
    inputs.output_depth = resources_.output_depth;
    inputs.output_velocity = resources_.output_velocity;
    const FrameStageDeclaration& producer = description.temporal_stage;
    const PassId first = producer.declare(graph, inputs, producer.user);
    if (first == kInvalidPass) {
        status_ =
            fail(ErrorCode::InvalidArgument,
                 "forward frame: the temporal stage's producer refused to declare its passes");
        return;
    }
    stage(FramePassKind::Temporal, "temporal", first);
    state.current_color = resources_.upscaled;
    state.stage_width = width;
    state.stage_height = height;
    if (valid(resources_.output_depth)) {
        state.stage_depth = resources_.output_depth;
        state.stage_velocity = resources_.output_velocity;
    }
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

    stage(FramePassKind::Shadow, "directional shadow", declare_shadow(graph, state));

    // 2. Depth prepass, and its MSAA resolve — "plus MSAA depth resolve if required", so that the
    // screen-space passes below work at single-sample resolution.
    if (features.depth_prepass) {
        stage(FramePassKind::DepthPrepass, "depth prepass", declare_prepass(graph, state));
        if (state.multisampled) {
            stage(FramePassKind::DepthResolve, "depth resolve",
                  declare_depth_resolve(graph, state));
        }
    }

    // 2c. Virtual geometry, into the depth the prepass left.
    if (features.virtual_geometry) {
        stage(FramePassKind::VirtualGeometry, "virtual geometry",
              declare_virtual_geometry(graph, state));
    }

    // 3. Cluster assignment: the device's producer, or the single pass.
    if (state.description->cluster_assignment_stage.declare != nullptr &&
        valid(resources_.cluster_headers)) {
        declare_produced_stage(graph, state, state.description->cluster_assignment_stage,
                               FramePassKind::ClusterAssignment, "cluster assignment",
                               resources_.cluster_headers);
    } else {
        stage(FramePassKind::ClusterAssignment, "cluster assignment",
              declare_cluster_assignment(graph, state));
    }

    // 4. The screen-space passes that need depth and normals.
    if (features.ambient_occlusion) {
        const FrameStageDeclaration& producer = state.description->ambient_occlusion_stage;
        if (producer.declare != nullptr) {
            declare_produced_stage(graph, state, producer, FramePassKind::AmbientOcclusion,
                                   "ambient occlusion", resources_.ambient_occlusion);
        } else {
            stage(FramePassKind::AmbientOcclusion, "ambient occlusion",
                  declare_screen_space(graph, state, "ambient occlusion",
                                       resources_.ambient_occlusion,
                                       FramePassKind::AmbientOcclusion));
        }
    }
    if (features.contact_shadows) {
        declare_produced_stage(graph, state, state.description->contact_shadows_stage,
                               FramePassKind::ContactShadows, "contact shadows",
                               resources_.contact_shadows);
    }
    if (features.screen_space_gi) {
        const FrameStageDeclaration& producer = state.description->screen_space_gi_stage;
        if (producer.declare != nullptr) {
            declare_produced_stage(graph, state, producer, FramePassKind::ScreenSpaceGi,
                                   "screen-space gi", resources_.screen_space_gi);
        } else {
            stage(FramePassKind::ScreenSpaceGi, "screen-space gi",
                  declare_screen_space(graph, state, "screen-space gi", resources_.screen_space_gi,
                                       FramePassKind::ScreenSpaceGi));
        }
    }

    // Volumetric fog, after the shadow pass whose map it reads.
    if (features.volumetric_fog) {
        declare_produced_stage(graph, state, state.description->volumetric_fog_stage,
                               FramePassKind::VolumetricFog, "volumetric fog",
                               resources_.volumetric_fog);
    }

    // The stochastic many-light path, last before shading: it reads the depth and the normals and
    // writes the two terms the opaque pass samples in place of the cluster loop.
    if (features.many_lights) {
        declare_produced_stage(graph, state, state.description->many_lights_stage,
                               FramePassKind::ManyLights, "many lights", resources_.many_lights);
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
        if (copies_opaque_colour(features) &&
            state.description->opaque_copy_stage.declare != nullptr) {
            declare_produced_stage(graph, state, state.description->opaque_copy_stage,
                                   FramePassKind::OpaqueColorCopy, "opaque colour copy",
                                   resources_.opaque_color_copy, state.shading_target);
        } else if (copies_opaque_colour(features)) {
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

    // 10. Temporal: a producer's passes — an upscaler, which reconstructs the output extent from
    // the render extent — or the single resolve.
    if (produced_temporal(description)) {
        declare_temporal_producer(graph, state);
    } else if (features.temporal) {
        PassBuilder builder = graph.add_pass("temporal", QueueKind::Graphics);
        builder.read(state.current_color, Access::FragmentSampledRead);
        builder.read(resources_.temporal_previous, Access::FragmentSampledRead);
        if (valid(resources_.velocity)) {
            builder.read(resources_.velocity, Access::FragmentSampledRead);
        }
        builder.read(resources_.depth, Access::FragmentSampledRead);
        builder.write(resources_.temporal_history, Access::ColorAttachmentWrite);
        attach(builder, description, FramePassKind::Temporal);
        stage(FramePassKind::Temporal, "temporal", builder.id());
        state.current_color = resources_.temporal_history;
    }

    // Fog of war: after the temporal resolve, on the converged image, and before every stage that
    // filters the picture — a lens blurs the fog like the ground it lies on, and bloom sees a
    // fogged light as the dimmer light it is. The producer reads the colour so far and the depth,
    // and writes a new full-resolution colour.
    if (features.fog_of_war) {
        TextureRequest request;
        request.name = "fog of war";
        request.format = description.color_format;
        request.width = output_width(description);
        request.height = output_height(description);
        request.extra_usage = rhi::TextureUsage::Storage;
        resources_.fog_of_war = graph.create_texture(request);
        declare_produced_stage(graph, state, description.fog_of_war_stage, FramePassKind::FogOfWar,
                               "fog of war", resources_.fog_of_war, state.current_color);
        state.current_color = resources_.fog_of_war;
    }

    // 7 of the post chain. Depth of field: after the temporal resolve, so it blurs a converged
    // image rather than a jittered one, and before bloom, so a defocused highlight blooms as the
    // disc it became. The producer reads the colour so far and writes a new full-resolution one.
    if (features.depth_of_field) {
        TextureRequest request;
        request.name = "depth of field";
        request.format = description.color_format;
        request.width = output_width(description);
        request.height = output_height(description);
        request.extra_usage = rhi::TextureUsage::Storage;
        resources_.depth_of_field = graph.create_texture(request);
        declare_produced_stage(graph, state, description.depth_of_field_stage,
                               FramePassKind::DepthOfField, "depth of field",
                               resources_.depth_of_field, state.current_color);
        state.current_color = resources_.depth_of_field;
    }

    // 8. Motion blur: after the temporal resolve and depth of field, whose colour it reads, and
    // before bloom, so a streak of a bright light blooms as the light it is. Its producer's last
    // pass writes the target and the chain continues from it.
    if (features.motion_blur) {
        resources_.motion_blur = description.motion_blur_target;
        declare_produced_stage(graph, state, description.motion_blur_stage,
                               FramePassKind::MotionBlur, "motion blur", resources_.motion_blur,
                               state.current_color);
        state.current_color = resources_.motion_blur;
    }

    // The interface's HDR composite: a document drawn onto the linear colour before bloom and the
    // tone curve, so an emissive element blooms like a light. Only with a producer; without one the
    // frame declares nothing here, as it never did.
    if (description.ui_hdr_stage.declare != nullptr) {
        declare_produced_stage(graph, state, description.ui_hdr_stage, FramePassKind::UiHdr,
                               "ui hdr", state.current_color);
    }

    // Bloom: scene-referred, after the temporal resolve and before the exposure the post-process
    // applies, which is what makes its threshold a physical quantity.
    if (features.bloom && status_) {
        status_ = declare_bloom_chain(
            graph, state.current_color, output_width(description), output_height(description),
            description.color_format, features.bloom_levels,
            description.callbacks[static_cast<u32>(FramePassKind::Bloom)], bloom_);
        for (u32 index = 0; index < bloom_.step_count; ++index) {
            stage(FramePassKind::Bloom, graph.pass_name(bloom_.steps[index].pass),
                  bloom_.steps[index].pass);
        }
        state.current_color = bloom_.output;
    }

    // 11. Post-process, which tonemaps STRAIGHT INTO THE OUTPUT. That is what removes the composite
    // blit in the ordinary case: the swapchain image is a colour attachment like any other.
    resources_.post_source = state.current_color;
    if (features.post_process) {
        PassBuilder builder = graph.add_pass("post-process", QueueKind::Graphics);
        builder.read(state.current_color, Access::FragmentSampledRead);
        builder.write(resources_.output, Access::ColorAttachmentWrite);
        attach(builder, description, FramePassKind::PostProcess);
        stage(FramePassKind::PostProcess, "post-process", builder.id());
        resources_.post_color = resources_.output;
        state.current_color = resources_.output;
    }

    // Selection outlines, over the tonemapped colour and under the interface. The producer's last
    // pass reads and writes the colour the chain ended in, so the interface below loads its result.
    if (features.selection_outlines) {
        declare_produced_stage(graph, state, description.selection_outlines_stage,
                               FramePassKind::SelectionOutlines, "selection outlines",
                               state.current_color);
    }

    // 12. UI and debug, drawn after tonemapping. A producer — the runtime interface's renderer —
    // declares its own passes over the colour the chain ended in, as the outlines above do.
    if (features.ui && description.ui_stage.declare != nullptr) {
        declare_produced_stage(graph, state, description.ui_stage, FramePassKind::UiAndDebug,
                               "ui and debug", state.current_color);
    } else if (features.ui) {
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
    state.stage_depth = resources_.depth;
    state.stage_velocity = resources_.velocity;
    state.stage_width = description.width;
    state.stage_height = description.height;

    declare_prepare_and_depth(graph, state);
    declare_shading(graph, state);
    declare_post_chain(graph, state);

    if (!status_) {
        return status_;
    }
    return graph.status();
}

Status ForwardFrame::check_output_extent(const FrameDescription& description) noexcept {
    const FrameFeatures& features = description.features;
    // A PRODUCER OF THE TEMPORAL STAGE IS FOR A TEMPORAL FRAME: without `features.temporal` the
    // frame has no stage to hand it, and declaring nothing would leave its history unwritten.
    if (description.temporal_stage.declare != nullptr && !features.temporal) {
        return fail(ErrorCode::InvalidArgument,
                    "forward frame: a temporal stage producer needs a temporal frame");
    }
    // ONE SAMPLE A PIXEL INTO THE PRODUCER: a reconstruction reads its jittered samples one texel
    // at a time, and a multisampled colour is resolved by a stage that does not run before it.
    if (description.temporal_stage.declare != nullptr && features.msaa_samples != 1) {
        return fail(ErrorCode::Unsupported,
                    "forward frame: a temporal stage producer reads single-sample targets");
    }
    const u32 width = output_width(description);
    const u32 height = output_height(description);
    if (width == description.width && height == description.height) {
        return ok();
    }
    // A FRAME THAT ENDS LARGER THAN IT WAS RENDERED NEEDS A RECONSTRUCTION. Nothing else in the
    // frame changes extent, so without a producer the post-process would read a smaller image than
    // it writes and stretch it.
    if (!produced_temporal(description)) {
        return fail(ErrorCode::InvalidArgument,
                    "forward frame: an output extent other than the render extent needs a temporal "
                    "stage producer to reconstruct one from the other");
    }
    if (width < description.width || height < description.height) {
        return fail(ErrorCode::InvalidArgument,
                    "forward frame: the output extent is smaller than the render extent");
    }
    return ok();
}

Status ForwardFrame::build(RenderGraph& graph, const FrameDescription& description) noexcept {
    passes_.clear();
    resources_ = FrameResources{};
    bloom_ = BloomChain{};
    status_ = ok();

    if (description.width == 0 || description.height == 0) {
        return fail(ErrorCode::InvalidArgument, "forward frame: the viewport is empty");
    }
    const u32 samples = description.features.msaa_samples;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) {
        return fail(ErrorCode::InvalidArgument, "forward frame: MSAA must be 1, 2, 4 or 8");
    }
    // ONE SAMPLE A PIXEL IS WHAT A VISIBILITY PAYLOAD MEANS. The resolve reconstructs a surface at
    // the pixel's centre from the one triangle the payload names; a multisampled target would need
    // a payload per sample and a resolve that shades them, and neither exists. Refused rather than
    // drawn into a single-sample target beside a multisampled depth, which would not validate.
    if (description.features.virtual_geometry && samples != 1) {
        return fail(ErrorCode::Unsupported, kVirtualGeometrySingleSample);
    }
    // EVERY SCREEN-SPACE AND TEMPORAL FEATURE READS THE PREPASS'S OUTPUT. Allowing one without a
    // prepass would declare a pass that samples a depth target nothing wrote — which compiles,
    // runs, and produces a frame that is subtly wrong rather than obviously broken. The
    // specification's own scenario ("SSAO enabled ... the prepass SHALL run in DepthNormal mode")
    // assumes the prepass is there, so this is that assumption stated as a refusal.
    const FrameFeatures& features = description.features;
    // A TEMPORAL STAGE A PRODUCER DECLARES IS THE EXCEPTION: a temporal upscaler reprojects a pixel
    // with no motion vector through the camera's own motion and its depth, which the opaque pass
    // writes when there is no prepass. A frame drawn by a caller whose geometry the prepass does
    // not walk — `samples/10-world` — is upscaled that way.
    const bool temporal_needs_prepass = features.temporal && !produced_temporal(description);
    const bool needs_prepass = features.ambient_occlusion || features.contact_shadows ||
                               features.screen_space_gi || features.screen_space_reflections ||
                               temporal_needs_prepass || features.motion_blur ||
                               features.normal_prepass;
    if (needs_prepass && !features.depth_prepass) {
        return fail(ErrorCode::InvalidArgument,
                    "forward frame: screen-space and temporal features require the depth prepass "
                    "that produces the depth, normal and velocity targets they read");
    }
    // THE CONTACT TERM HAS NO SINGLE-PASS STAND-IN. Its producer owns the target the opaque pass
    // samples, so a frame asked for contact shadows without one would read an image nothing wrote.
    if (features.contact_shadows && (description.contact_shadows_stage.declare == nullptr ||
                                     !valid(description.contact_shadows_target))) {
        return fail(ErrorCode::InvalidArgument,
                    "forward frame: contact shadows need their producer's stage and target");
    }
    // A PRODUCED SCREEN-SPACE GI STAGE NEEDS ITS TARGET: the opaque pass samples the producer's
    // texture, so a producer without one would leave the read on a resource nothing declared.
    if (features.screen_space_gi && description.screen_space_gi_stage.declare != nullptr &&
        !valid(description.screen_space_gi_target)) {
        return fail(ErrorCode::InvalidArgument,
                    "forward frame: a screen-space GI producer needs its diffuse target");
    }
    // NOR DOES THE FOG VOLUME, for the same reason: its producer owns the texture the opaque pass
    // samples.
    if (features.volumetric_fog && (description.volumetric_fog_stage.declare == nullptr ||
                                    !valid(description.volumetric_fog_target))) {
        return fail(ErrorCode::InvalidArgument,
                    "forward frame: volumetric fog needs its producer's stage and target");
    }
    // NOR DOES THE STOCHASTIC MANY-LIGHT PATH: the opaque pass samples both of its producer's
    // targets in place of the cluster loop, and it reconstructs every surface from the prepass.
    if (features.many_lights) {
        if (description.many_lights_stage.declare == nullptr ||
            !valid(description.many_lights_target) ||
            !valid(description.many_lights_reservoir_target)) {
            return fail(ErrorCode::InvalidArgument,
                        "forward frame: the many-light path needs its producer's stage and both "
                        "of its targets");
        }
        if (!features.depth_prepass) {
            return fail(ErrorCode::InvalidArgument,
                        "forward frame: the many-light path reconstructs every surface from the "
                        "depth prepass, and this frame has none");
        }
    }
    // THE DEVICE'S CLUSTER LISTS COME AS A PAIR: a header array whose runs index a buffer the
    // frame did not bind is a frame reading somebody else's words.
    if (valid(description.cluster_headers_target) != valid(description.cluster_indices_target)) {
        return fail(
            ErrorCode::InvalidArgument,
            "forward frame: the device's cluster headers and indices are imported together");
    }
    // NOR DOES MOTION BLUR. The gather reads the tile reductions its producer declares, and the
    // chain continues from the producer's target: without one, post-processing would read an image
    // nothing wrote.
    if (features.motion_blur && (description.motion_blur_stage.declare == nullptr ||
                                 !valid(description.motion_blur_target))) {
        return fail(ErrorCode::InvalidArgument,
                    "forward frame: motion blur needs its producer's stage and target");
    }
    // THE OUTLINE STAGE HAS NO SINGLE-PASS STAND-IN EITHER, and it reads the prepass depth: a
    // marked surface is "hidden" where the scene's depth is nearer than its own, and a frame with
    // no prepass has no single-sample depth to ask.
    if (features.selection_outlines) {
        if (description.selection_outlines_stage.declare == nullptr) {
            return fail(ErrorCode::InvalidArgument,
                        "forward frame: selection outlines need their producer's stage");
        }
        if (!features.depth_prepass) {
            return fail(ErrorCode::InvalidArgument,
                        "forward frame: selection outlines compare marked surfaces with the depth "
                        "prepass, and this frame has none");
        }
    }
    // DEPTH OF FIELD HAS NO SINGLE-PASS STAND-IN, and it reads the single-sample depth: with MSAA
    // that is the prepass's resolve, which a frame without the prepass does not declare.
    if (features.depth_of_field) {
        if (description.depth_of_field_stage.declare == nullptr) {
            return fail(ErrorCode::InvalidArgument,
                        "forward frame: depth of field needs its producer's stage");
        }
        if (samples > 1 && !features.depth_prepass) {
            return fail(ErrorCode::InvalidArgument,
                        "forward frame: depth of field reads the single-sample depth, which a "
                        "multisampled frame has only after the depth prepass's resolve");
        }
    }
    // FOG OF WAR HAS NO SINGLE-PASS STAND-IN, and it reads the single-sample depth to find where
    // each pixel lies on the grid: with MSAA that is the prepass's resolve.
    if (features.fog_of_war) {
        if (description.fog_of_war_stage.declare == nullptr) {
            return fail(ErrorCode::InvalidArgument,
                        "forward frame: fog of war needs its producer's stage");
        }
        if (samples > 1 && !features.depth_prepass) {
            return fail(ErrorCode::InvalidArgument,
                        "forward frame: fog of war reads the single-sample depth, which a "
                        "multisampled frame has only after the depth prepass's resolve");
        }
    }
    if (Status extents = check_output_extent(description); !extents) {
        return extents;
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
