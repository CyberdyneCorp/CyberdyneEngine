// The frame, assembled. See cy/rendering/assembly/frame_assembly.h for the argument and the order;
// this file is the plumbing.

#include <cy/core/math/projection.h>
#include <cy/rendering/assembly/frame_assembly.h>

#include <cmath>

namespace cy::rendering::assembly {
namespace {

using render::SortLayer;

/// The default surface query: ONE opaque surface per visible instance, whose material is the
/// instance's GPU slot.
///
/// Enough to assemble and sort a frame, and not enough to shade one — which is why `FrameSinks`
/// documents it as the fallback rather than as a default anybody should ship. A caller with a mesh
/// table supplies its own and this is never called.
Span<const DrawSurface> default_surfaces(const VisibleInstance& instance, void* /*user*/) noexcept {
    // Function-local rather than a member, because `SurfaceQueryFn` is a plain function pointer and
    // the span it returns is borrowed only until the next call — `build_draw_list` copies out of it
    // before asking for the next instance's, which is exactly the contract draw_list.h states.
    static thread_local DrawSurface surface;
    surface = DrawSurface{};
    // The SPATIAL slot, not the GPU one: nothing here owns a GPU scene, so `gpu_slot` is whatever
    // the publisher put in the entry and is zero for a `SceneIndex`. `SceneIndex::surface_of` is
    // what a caller with a real mesh table resolves through, and it is keyed by the spatial slot.
    surface.mesh = instance.slot;
    surface.material = instance.slot;
    surface.pipeline = 0;
    surface.blend = render::BlendMode::Opaque;
    return {&surface, 1};
}

/// A view-space position, from a world-space one and the view matrix.
[[nodiscard]] Vec3 to_view_space(const Mat4& view, Vec3 world) noexcept {
    const Vec4 transformed = view * Vec4{world.x, world.y, world.z, 1.0F};
    return Vec3{transformed.x, transformed.y, transformed.z};
}

/// The cluster element one light becomes.
[[nodiscard]] ClusterElement element_of(const render::LightDescription& light, const Mat4& view,
                                        u32 payload_index) noexcept {
    ClusterElement element;
    element.view_position = to_view_space(view, light.transform.translation);
    element.radius = light_bounding_radius(light);
    element.payload_index = payload_index;
    element.layer_mask = light.layer_mask;
    element.type = ClusterElementType::Light;
    if (light.kind == render::LightKind::Spot && element_is_cone(light.outer_cone_radians)) {
        const Vec3 axis = light.transform.rotation * Vec3{0.0F, 0.0F, -1.0F};
        const Vec4 rotated = view * Vec4{axis.x, axis.y, axis.z, 0.0F};
        element.view_direction = Vec3{rotated.x, rotated.y, rotated.z};
        element.cone_cos = std::cos(light.outer_cone_radians);
    }
    return element;
}

/// The update class a light's shadow pages are asked for under.
///
/// A directional light's cascades cover the whole view and go stale the moment the camera moves, so
/// they are `Dynamic`; a punctual light's pages are `Normal`. Both are the shadow module's own
/// vocabulary and neither is a number invented here.
[[nodiscard]] UpdateClass update_class_of(render::LightKind kind) noexcept {
    return kind == render::LightKind::Directional ? UpdateClass::Dynamic : UpdateClass::Normal;
}

/// How many clip levels a light's shadow occupies. One page per level, which is the coarse request
/// a frame makes before receiver marking narrows it — `shadows/pages.h` owns the narrowing.
inline constexpr u8 kShadowLevels = 3;

}  // namespace

FrameAssembly::FrameAssembly(Allocator& allocator) noexcept
    : allocator_(&allocator),
      workspace_(allocator),
      results_(allocator),
      previous_lod_levels_(allocator),
      draws_(allocator),
      sort_(allocator),
      lights_(allocator),
      elements_(allocator),
      clusters_(allocator),
      materials_(allocator),
      shadows_(allocator),
      sky_(allocator),
      temporal_(allocator),
      frame_(allocator),
      published_(allocator) {}

FrameAssembly::~FrameAssembly() {
    vt_frame_.destroy();
    cull_pass_.destroy();
}

Status FrameAssembly::initialize(const AssemblyDescription& description) noexcept {
    // Each of these produces a frame that compiles and renders nothing, which is the failure this
    // module exists to stop being invisible.
    if (description.width == 0 || description.height == 0) {
        return fail(ErrorCode::InvalidArgument, "a frame needs a viewport");
    }
    if (!std::isfinite(description.far_plane) || description.far_plane <= description.near_plane) {
        return fail(ErrorCode::InvalidArgument,
                    "the cluster grid's slice mapping is logarithmic in far/near, so the far plane "
                    "must be finite and beyond the near one");
    }
    if (description.material_capacity == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a frame with no material slots resolves every draw to nothing");
    }
    description_ = description;

    const Expected<ClusterGrid, Error> grid =
        make_cluster_grid(description.clusters, description.width, description.height,
                          description.near_plane, description.far_plane);
    if (!grid) {
        return make_unexpected(grid.error());
    }
    grid_ = *grid;

    if (Status ready = materials_.initialize(description.material_capacity); !ready) {
        return ready;
    }
    if (Status ready = shadows_.initialize(description.shadows); !ready) {
        return ready;
    }
    if (Status ready = sky_.configure(description.sky); !ready) {
        return ready;
    }
    if (Status ready = temporal_.initialize(description.temporal); !ready) {
        return ready;
    }
    // ONE CONSUMER, and it needs jitter: the temporal stage of the post chain. `needs_jitter` is
    // what makes the projection unjittered when nothing temporal is on, and a frame that registered
    // no consumer at all would get no jitter even with TAA enabled.
    const Expected<ConsumerId, Error> consumer =
        temporal_.register_consumer("frame-assembly", true);
    if (!consumer) {
        return make_unexpected(consumer.error());
    }
    temporal_consumer_ = *consumer;
    HistoryDeclaration history;
    history.format = HistoryFormat::Rgba16F;
    const Expected<HistoryId, Error> declared =
        temporal_.declare_history(temporal_consumer_, history);
    if (!declared) {
        return make_unexpected(declared.error());
    }
    history_ = *declared;

    initialized_ = true;
    return ok();
}

Status FrameAssembly::attach_device(rhi::Device& device) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::InvalidArgument, "attach a device after initialize()");
    }
    device_ = &device;
    if (!description_.gpu_culling || !gpu_culling::GpuCullPass::supported(device)) {
        // NOT AN ERROR. A device with no compute queue or no indirect drawing gets the CPU path,
        // and cull.h's own comment says why that is the same answer rather than a lesser one.
        cull_pass_ready_ = false;
        return ok();
    }
    gpu_culling::GpuCullPassDescription desc;
    desc.max_instances = description_.max_instances;
    desc.max_draws = description_.max_draws;
    desc.max_lod_chains = description_.max_lod_chains;
    desc.max_mesh_lods = description_.max_mesh_lods;
    if (Status created = cull_pass_.create(*allocator_, device, desc); !created) {
        return created;
    }
    cull_pass_ready_ = true;
    return ok();
}

Status FrameAssembly::attach_virtual_texture(const vt::VirtualTextureDesc& desc,
                                             const vt::FeedbackSettings& settings) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "attach a device before a virtual texture");
    }
    if (Status created = vt_frame_.create(*allocator_, *device_, desc, settings); !created) {
        return created;
    }
    vt_ready_ = true;
    return ok();
}

Span<const render::DrawItem> FrameAssembly::layer(SortLayer sort_layer) const noexcept {
    const auto index = static_cast<usize>(sort_layer);
    if (index >= static_cast<usize>(SortLayer::Count)) {
        return {};
    }
    const u32 first = layer_begin_[index];
    const u32 last = layer_begin_[index + 1];
    return {draws_.items.data() + first, last - first};
}

// --- 1. What the frame has switched on -------------------------------------------------------

Status FrameAssembly::decide_features(const AssemblyView& /*view*/, FrameFeatures& features,
                                      AssemblyReport& out) const noexcept {
    PostChainConfig config = description_.post;
    // The prepass is what produces motion vectors, and it is derived from the feature set rather
    // than set — so the honest thing to tell the chain is "the frame will produce them if you need
    // them", which is exactly what `may_enable_motion_vectors` means.
    config.motion_vectors_available =
        config.temporal_antialiasing || config.temporal_upscaling || config.motion_blur;
    const PostChainResult chain =
        build_post_chain(config, out.post_stage, static_cast<u32>(kMaxPostStages));
    out.post_stages = chain.count;
    out.post_refusal = chain.refusal;
    if (!chain.ok()) {
        // A refusal is a configuration error and is reported as one, naming the stage pair. The
        // alternative — quietly dropping a stage the project asked for — is how a project ships
        // without the antialiasing somebody enabled.
        return fail(ErrorCode::InvalidArgument, chain.diagnostic);
    }

    features = FrameFeatures{};
    features.ambient_occlusion = config.ambient_occlusion;
    features.screen_space_reflections = config.screen_space_reflections;
    features.temporal = config.temporal_antialiasing || config.temporal_upscaling;
    features.motion_blur = config.motion_blur;
    features.post_process = chain.count != 0;
    return ok();
}

// --- 3. Culling -------------------------------------------------------------------------------

Status FrameAssembly::run_cull(const SpatialIndex& index, const AssemblyView& view,
                               AssemblyReport& out) noexcept {
    CullOptions options;
    if (Status culled = cull_view(index, view.cull, options, workspace_, results_); !culled) {
        return culled;
    }
    // Hysteresis state is indexed by spatial slot and belongs to the caller for the reason lod.h
    // gives; the assembly is that caller, and it keeps one band per view across frames.
    if (previous_lod_levels_.size() < index.slot_count()) {
        if (Status sized = previous_lod_levels_.resize(index.slot_count()); !sized) {
            return sized;
        }
    }
    out.cull = results_.stats;
    out.culled_on_device = false;
    return ok();
}

Status FrameAssembly::run_device_cull(const SpatialIndex& index, const AssemblyView& view,
                                      AssemblyReport& out) noexcept {
    if (Status published = publish_gpu_cull_scene(index, published_); !published) {
        return published;
    }
    render::culling::GpuCullView gpu_view;
    write_gpu_cull_view(view.cull, published_.high_water, gpu_view);

    render::culling::GpuCullScene scene;
    scene.instances = published_.instances.span();
    scene.chains = view.lod_chains;
    scene.mesh_lods = view.mesh_lods;
    scene.ranges = published_.any_ranges ? published_.ranges.span()
                                         : Span<const render::culling::GpuVisibilityRange>();
    if (Status uploaded = cull_pass_.upload(scene, gpu_view); !uploaded) {
        return uploaded;
    }
    out.culled_on_device = true;
    return ok();
}

// --- 4. The draw list -------------------------------------------------------------------------

Status FrameAssembly::build_draws(const FrameSinks& sinks, AssemblyReport& out) noexcept {
    draws_.clear();
    const SurfaceQueryFn query = sinks.surfaces != nullptr ? sinks.surfaces : &default_surfaces;
    void* user = sinks.surfaces != nullptr ? sinks.surfaces_user : nullptr;

    // Opaque and transparent in one list, because the sort key's LAYER field is what separates
    // them and a second list would be a second ordering to keep in step. `layer()` slices it.
    if (Status built = build_draw_list(results_.opaque.span(), query, user, draws_); !built) {
        return built;
    }
    out.opaque = static_cast<u32>(draws_.items.size());
    DrawList transparent(*allocator_);
    if (Status built = build_draw_list(results_.transparent.span(), query, user, transparent);
        !built) {
        return built;
    }
    // The two lists JOIN, items and records together, because `sort_draw_list` moves them in step
    // and draw i's record is instances[i] afterwards. Two lists would be two orderings.
    for (usize index = 0; index < transparent.items.size(); ++index) {
        if (Status added = draws_.items.push_back(transparent.items[index]); !added) {
            return added;
        }
        if (Status added = draws_.instances.push_back(transparent.instances[index]); !added) {
            return added;
        }
    }
    out.transparent = static_cast<u32>(transparent.items.size());

    if (Status sorted = sort_draw_list(draws_, sort_); !sorted) {
        return sorted;
    }

    // The layer table, so `layer()` is a subspan rather than a search. A count and a prefix sum:
    // the sort key's most significant field is the layer, so the counts ARE the runs, and a layer
    // with no draws gets an empty range at the right place instead of a special case.
    u32 counts[static_cast<usize>(SortLayer::Count)] = {};
    for (const render::DrawItem& item : draws_.items) {
        counts[static_cast<usize>(render::sort_key_layer(item.key))] += 1;
    }
    u32 running = 0;
    for (usize slot = 0; slot < static_cast<usize>(SortLayer::Count); ++slot) {
        layer_begin_[slot] = running;
        running += counts[slot];
    }
    layer_begin_[static_cast<usize>(SortLayer::Count)] = running;

    out.draws = static_cast<u32>(draws_.items.size());
    out.batches = static_cast<u32>(draws_.batches.size());
    return check_materials(out);
}

/// Every draw's material index, against the table it indexes.
///
/// THIS IS THE JOIN M7's GATE WAS ABOUT, and it is the one that reads worst if it is missing: "the
/// material compiler fills a GPU material table nothing draws with". A draw carries a slot index
/// and the table is `cy::rendering-material`'s; a frame that never compared the two would link the
/// module, report its capacity, and still be drawing with whatever number the surface query
/// happened to produce.
///
/// It COUNTS rather than refuses. A draw whose slot is past the table's end shades with slot zero
/// on the device — the buffer's length is what the descriptor says — and that is a frame with the
/// wrong material on one object, not a frame that must not be rendered. `AssemblyReport` carries
/// the number so it is visible; the alternative, dropping the draw, would make an object disappear
/// for a shading mistake.
Status FrameAssembly::check_materials(AssemblyReport& out) noexcept {
    out.material_slots = materials_.capacity();
    out.material_slots_live = materials_.live();
    out.draws_without_material = 0;
    // `GpuDrawInstance::material` and not `DrawItem`: the sort key packs a material's identity for
    // ORDERING and the record carries the slot the shader indexes, and it is the second of the two
    // a draw actually shades with.
    for (const GpuDrawInstance& record : draws_.instances) {
        if (record.material >= materials_.capacity()) {
            out.draws_without_material += 1;
        }
    }
    // What the caller has to transfer before this frame shades: one interval, because
    // `MaterialTable::dirty_range` is one interval by design and "changed materials SHALL be
    // collected during Prepare and uploaded in one transfer".
    out.material_upload_offset = 0;
    out.material_upload_size = 0;
    (void)materials_.dirty_range(out.material_upload_offset, out.material_upload_size);
    return ok();
}

// --- 5. Lights and clusters -------------------------------------------------------------------

Status FrameAssembly::build_lights(const AssemblyView& view, AssemblyReport& out) noexcept {
    lights_.clear();
    elements_.clear();
    LightBuildParameters parameters;
    parameters.camera_origin = view.cull.camera_position;
    for (usize index = 0; index < view.lights.size(); ++index) {
        const render::LightDescription& light = view.lights[index];
        parameters.unit = default_unit_for(light.kind);
        if (Status added = lights_.push_back(build_gpu_light(light, parameters)); !added) {
            return added;
        }
        // A directional light has an infinite bound and is not clustered: it reaches every cluster,
        // and putting it in every list would be the whole grid holding one index.
        if (light.kind == render::LightKind::Directional) {
            continue;
        }
        if (Status added =
                elements_.push_back(element_of(light, view.view, static_cast<u32>(index)));
            !added) {
            return added;
        }
    }
    out.lights = static_cast<u32>(lights_.size());

    const f32 tan_half_fov = std::tan(view.fov_y_radians * 0.5F);
    const f32 aspect = static_cast<f32>(description_.width) / static_cast<f32>(description_.height);
    if (Status assigned = assign_clusters(grid_, elements_.span(), view.cull.layer_mask,
                                          tan_half_fov, aspect, clusters_);
        !assigned) {
        return assigned;
    }
    out.clusters = clusters_.stats;
    return ok();
}

// --- 6. Shadow pages --------------------------------------------------------------------------

Status FrameAssembly::request_shadow_pages(const AssemblyView& view, AssemblyReport& out) noexcept {
    shadows_.begin_frame(frame_index_);
    for (usize index = 0; index < view.lights.size(); ++index) {
        const render::LightDescription& light = view.lights[index];
        if (!light.casts_shadow) {
            continue;
        }
        for (u8 level = 0; level < kShadowLevels; ++level) {
            VirtualPage page;
            page.light_slot = static_cast<u32>(index);
            page.level = level;
            const PageLookup lookup = shadows_.request(page, update_class_of(light.kind));
            out.shadow_pages_requested += 1;
            // A cache HIT is a page that needs no rasterising this frame — which is the number a
            // shadow budget is spent against, and it is `needs_render` inverted rather than a
            // second word to keep in step.
            out.shadow_pages_resident += (!lookup.needs_render && !lookup.starved) ? 1U : 0U;
        }
    }
    return ok();
}

// --- 7. The sky -------------------------------------------------------------------------------

Status FrameAssembly::update_sky(const AssemblyView& view, AssemblyReport& out) noexcept {
    const Expected<bool, Error> rebuilt =
        sky_.update(view.atmosphere, view.cull.camera_position, view.sun_direction);
    if (!rebuilt) {
        return make_unexpected(rebuilt.error());
    }
    out.sky_rebuilt = *rebuilt;
    // Upward-facing, which is the ambient term a frame's sky contributes to a horizontal surface —
    // the reference `fit_sky_gradient` is measured against and the number a light meter would read.
    sky_irradiance_ = sky::sky_irradiance(view.atmosphere, view.cull.camera_position,
                                          view.sun_direction, Vec3{0.0F, 1.0F, 0.0F});
    return ok();
}

// --- 8. The frame -----------------------------------------------------------------------------

Status FrameAssembly::declare_frame(const AssemblyView& view, const FrameFeatures& features,
                                    const FrameSinks& sinks, RenderGraph& graph,
                                    AssemblyReport& out) noexcept {
    // The device passes go in FIRST. They produce what the frame's passes consume — the cull's
    // indirect arguments and the virtual texture's page table — and the graph derives the ordering
    // from the declarations rather than from the order they were added, so this is about reading
    // order rather than about correctness.
    cull_pass_declared_ = false;
    if (out.culled_on_device) {
        if (Status declared = cull_pass_.declare(graph); !declared) {
            return declared;
        }
        cull_pass_declared_ = true;
    }
    vt_declared_ = false;
    if (vt_ready_ && view.page_table != nullptr) {
        if (Status uploaded = vt_frame_.upload_page_table(*view.page_table); !uploaded) {
            return uploaded;
        }
        if (Status declared = vt_frame_.declare(graph); !declared) {
            return declared;
        }
        vt_declared_ = true;
        out.virtual_texture_declared = true;
    }

    FrameDescription description;
    description.width = description_.width;
    description.height = description_.height;
    description.color_format = description_.color_format;
    description.depth_format = description_.depth_format;
    description.features = features;
    description.cluster_grid = grid_;
    description.light_count = static_cast<u32>(lights_.size());
    description.draw_instance_count = static_cast<u32>(draws_.instances.size());
    description.output = view.output;
    description.cluster_queue = description_.cluster_queue;
    for (u32 kind = 0; kind < kFramePassKindCount; ++kind) {
        description.callbacks[kind] = sinks.passes[kind];
    }

    if (Status built = frame_.build(graph, description); !built) {
        return built;
    }
    out.prepass = frame_.prepass_mode();
    out.passes_declared = static_cast<u32>(frame_.passes().size());
    return ok();
}

// --- The whole of it --------------------------------------------------------------------------

Status FrameAssembly::assemble(const SpatialIndex& index, const AssemblyView& view,
                               const FrameSinks& sinks, RenderGraph& graph,
                               AssemblyReport& out) noexcept {
    if (!initialized_) {
        return fail(ErrorCode::InvalidArgument, "the assembly was never initialized");
    }
    out = AssemblyReport{};
    ++frame_index_;

    FrameFeatures features;
    if (Status decided = decide_features(view, features, out); !decided) {
        return decided;
    }

    TemporalView temporal_view;
    temporal_view.width = description_.width;
    temporal_view.height = description_.height;
    temporal_view.view = view.view;
    temporal_view.projection = view.projection;
    temporal_view.camera_position = view.cull.camera_position;
    if (view.cut) {
        temporal_.signal_cut(TemporalInvalidation::Explicit);
    }
    temporal_.begin_frame(temporal_view);
    out.temporal_frame = temporal_.frame();
    out.temporal_invalidated = temporal_.invalidated_this_frame();

    const bool on_device = cull_pass_ready_ && device_ != nullptr;
    if (on_device) {
        if (Status culled = run_device_cull(index, view, out); !culled) {
            return culled;
        }
        // The survivors are not known until the dispatch has run, so the draw list is empty at
        // declaration time and the frame's own passes read the indirect arguments instead. The CPU
        // path below is what a caller that needs the list on the CPU asks for by turning
        // `gpu_culling` off.
        results_.clear();
    } else if (Status culled = run_cull(index, view, out); !culled) {
        return culled;
    }

    if (Status built = build_draws(sinks, out); !built) {
        return built;
    }
    if (Status built = build_lights(view, out); !built) {
        return built;
    }
    if (Status requested = request_shadow_pages(view, out); !requested) {
        return requested;
    }
    if (Status updated = update_sky(view, out); !updated) {
        return updated;
    }
    return declare_frame(view, features, sinks, graph, out);
}

Status FrameAssembly::execute(GraphExecutor& executor, RenderGraph& graph,
                              AssemblyReport& out) noexcept {
    if (Status declared = graph.status(); !declared) {
        return declared;
    }
    CompileOptions compile;
    ExecuteOptions execute_options;
    const Expected<ExecutionResult, Error> result =
        executor.execute(graph, compile, execute_options);
    if (!result) {
        return make_unexpected(result.error());
    }
    out.execution = *result;
    out.executed = true;

    // The read-backs, after the submit. `GpuCullPass::read_back` says it MUST follow the frame's
    // fence; `execute` waits, which is what makes this legal here rather than a race that shows up
    // as an empty draw list once in fifty runs.
    if (device_ != nullptr && (cull_pass_declared_ || vt_declared_)) {
        if (Status idle = device_->wait_idle(); !idle) {
            return idle;
        }
    }
    if (cull_pass_declared_) {
        const Expected<gpu_culling::GpuCullReadback, Error> readback = cull_pass_.read_back();
        if (!readback) {
            return make_unexpected(readback.error());
        }
        out.cull.visible = readback->counters.visible;
        out.cull.tested = readback->counters.tested;
        out.cull.rejected_by_frustum = readback->counters.rejected_by_frustum;
        out.cull.rejected_by_layer = readback->counters.rejected_by_layer;
        out.cull.rejected_by_range = readback->counters.rejected_by_range;
        out.draws = static_cast<u32>(readback->commands.size());
    }
    if (vt_declared_) {
        const Expected<vt::VirtualTextureFrameReadback, Error> feedback = vt_frame_.read_back();
        if (!feedback) {
            return make_unexpected(feedback.error());
        }
        out.virtual_texture_requests = static_cast<u32>(feedback->requests.size());
        out.virtual_texture_dropped = feedback->dropped;
        out.virtual_texture_bytes_read = feedback->bytes_read;
    }
    return ok();
}

}  // namespace cy::rendering::assembly
