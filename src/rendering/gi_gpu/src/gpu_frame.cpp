// SPDX-License-Identifier: MIT
#include <cy/rendering/gi_gpu/gpu_frame.h>

#include <cy/rendering/graph/executor.h>
#include <cy/rendering/raytracing_gpu/device_structures.h>

#include <algorithm>

namespace cy::rendering::gi_gpu {
namespace {

/// Every probe slot the system's clipmap can hold: a cube of cells a level.
[[nodiscard]] u32 clipmap_probes(const gi::IlluminationSystem& system) noexcept {
    const gi::ProbeCacheSettings& probes = system.radiance().settings();
    const u32 side = (probes.half_extent_probes * 2U) + 1U;
    const u32 lattice = probes.levels * side * side * side;
    return std::max(lattice, static_cast<u32>(system.radiance().probes().size()));
}

/// The field's capacity: every brick of every level's window.
[[nodiscard]] GpuGiSceneDescription scene_for(const gi::IlluminationSystem& system,
                                              GpuGiSceneDescription scene) noexcept {
    const gi::ClipmapSettings& field = system.settings().field;
    const u32 bricks = std::max(field.resolution / 4U, 1U);
    scene.max_levels = std::max(scene.max_levels, field.levels);
    scene.max_window_bricks = std::max(scene.max_window_bricks, bricks);
    scene.max_brick_slots =
        std::max(scene.max_brick_slots, field.levels * bricks * bricks * bricks);
    // Room for the cards a running world adds: what it holds now, twice over.
    scene.max_cards = std::max(scene.max_cards, (system.surfaces().page_count() * 2U) + 1024U);
    return scene;
}

}  // namespace

GpuGiFrame::GpuGiFrame() noexcept = default;

GpuGiFrame::~GpuGiFrame() {
    destroy();
}

bool GpuGiFrame::supported(const rhi::Device& device) noexcept {
    return GpuScreenResolve::supported(device);
}

Status GpuGiFrame::create(Allocator& allocator, rhi::Device& device, gi::IlluminationSystem& system,
                          const GpuGiFrameDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device GI frame is already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "the device GI needs compute and a shader format it ships");
    }
    if (desc.width == 0 || desc.height == 0) {
        return fail(ErrorCode::InvalidArgument, "GpuGiFrameDescription: a frame has pixels");
    }
    allocator_ = &allocator;
    device_ = &device;
    system_ = &system;
    desc_ = desc;
    desc_.cache.max_probes = desc.max_probes != 0 ? desc.max_probes : clipmap_probes(system);
    Status made = scene_.create(allocator, device, scene_for(system, desc.scene));
    if (made) {
        made = cache_.create(allocator, device, scene_, desc_.cache);
    }
    if (made) {
        cache_.bind_field(system.field());
        // A host-shaded surface cache's radiance is uploaded for the gather; a device-shaded one's
        // is already there.
        cache_.bind_host_surfaces(desc.device_caches ? nullptr : &system.surfaces());
        made = create_hardware();
    }
    if (made && desc.device_caches) {
        shading_.bind(scene_, system.field(), system.surfaces().lookup_radius());
        system.set_surface_shading(&shading_);
        system.set_probe_gather(&cache_);
    }
    if (made) {
        GpuScreenResolveDescription resolve;
        resolve.width = desc.width;
        resolve.height = desc.height;
        resolve.readback = desc.readback;
        made = resolve_.create(allocator, device, cache_, resolve);
    }
    if (made && desc.debug_views) {
        GpuGiDebugViewDescription views;
        views.readback = desc.readback;
        made = views_.create(allocator, device, resolve_, views);
    }
    if (!made) {
        destroy();
        return made;
    }
    resolve_.set_denoiser(system.denoiser());
    resolve_.set_field(system.field());
    resolve_.set_sky(system.sky_term());
    resolve_.set_timing(true);
    if (desc.device_caches && device.capabilities().has(rhi::Capability::TimestampQueries)) {
        rhi::QueryPoolDescription timer;
        timer.name = "gi caches timestamps";
        timer.kind = rhi::QueryKind::Timestamp;
        timer.count = 3;
        if (Expected<rhi::QueryPoolHandle, Error> pool = device.create_query_pool(timer); pool) {
            timer_ = *pool;
        }
    }
    return ok();
}

Status GpuGiFrame::create_hardware() noexcept {
    hardware_ = false;
    if (!desc_.hardware || !rt_gpu::DeviceStructures::supported(*device_) ||
        system_->acceleration().diagnostics().instance_count == 0) {
        return ok();
    }
    structures_ = std::make_unique<rt_gpu::DeviceStructures>();
    if (Status made = structures_->create(*allocator_, *device_, {}); !made) {
        structures_.reset();
        return made;
    }
    system_->acceleration().set_mirror(structures_.get());
    (void)system_->acceleration().update();
    // THE STRUCTURES ARE BUILT BEFORE THE FIRST FRAME, so the first frame's ray queries find a
    // traceable scene — `samples/12-beauty`'s order, in a device frame of their own.
    if (!device_->begin_frame().has_value()) {
        return fail(ErrorCode::Internal, "the GI's structure frame did not begin");
    }
    RenderGraph graph(*allocator_);
    Status declared = structures_->declare(graph);
    if (declared) {
        GraphExecutor executor(*allocator_, *device_);
        auto executed = executor.execute(graph, CompileOptions{}, ExecuteOptions{});
        if (!executed) {
            declared = make_unexpected(executed.error());
        }
    }
    (void)device_->wait_idle();
    (void)device_->end_frame();
    if (!declared) {
        return declared;
    }
    if (Status bound = cache_.bind_hardware(&system_->acceleration(), structures_.get()); !bound) {
        return bound;
    }
    hardware_ = true;
    return ok();
}

void GpuGiFrame::destroy() noexcept {
    if (device_ != nullptr) {
        (void)device_->wait_idle();
    }
    if (system_ != nullptr && structures_ != nullptr &&
        system_->acceleration().mirror() == structures_.get()) {
        system_->acceleration().set_mirror(nullptr);
    }
    if (system_ != nullptr && desc_.device_caches) {
        // The host shades and gathers again from the next update.
        system_->set_surface_shading(nullptr);
        system_->set_probe_gather(nullptr);
    }
    shading_declared_ = ~u64{0};
    gather_declared_ = ~u64{0};
    if (device_ != nullptr && timer_) {
        device_->destroy_query_pool(timer_);
    }
    timer_ = {};
    timer_written_ = false;
    views_.destroy();
    resolve_.destroy();
    cache_.destroy();
    scene_.destroy();
    if (structures_ != nullptr) {
        structures_->destroy();
        structures_.reset();
    }
    view_settings_ = GpuGiDebugViewSettings{};
    hardware_ = false;
    system_ = nullptr;
    device_ = nullptr;
    allocator_ = nullptr;
}

Status GpuGiFrame::set_debug_view(const GpuGiDebugViewSettings& settings) noexcept {
    if (system_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device GI frame is not created");
    }
    if (settings.view != gi::GiDebugView::Count) {
        if (!views_.created()) {
            return fail(ErrorCode::Unsupported, "the device GI frame was created without views");
        }
        if (!GpuGiDebugView::draws(settings.view, *system_)) {
            return fail(ErrorCode::Unsupported,
                        "the device does not hold this GI view's data; draw it on the host");
        }
    }
    view_settings_ = settings;
    return ok();
}

Status GpuGiFrame::prepare(RenderGraph& graph, const GpuGiFrameView& view, u64 frame,
                           GpuGiFrameTargets& out) noexcept {
    out = GpuGiFrameTargets{};
    if (system_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device GI frame is not created");
    }
    const gi::IlluminationSystem& system = *system_;
    // Last frame's cost into the budget, and the budget's levers onto this frame's resolve.
    budget_.observe(last_cost());
    gi::ScreenResolveSettings settings = resolve_.settings();
    settings.mode = system.runtime_settings().mode;
    settings.max_ray_distance_metres = system.settings().max_ray_distance_metres;
    settings.reflection_thresholds = system.settings().reflection_thresholds;
    apply_levers(system, settings, resolve_);
    resolve_.set_sky(system.sky_term());
    if (Status synced = resolve_.sync(system.radiance()); !synced) {
        return synced;
    }
    resolve_.set_cache_frame(frame);
    GpuResolveView resolve_view;
    resolve_view.camera = view.eye;
    resolve_view.world_to_clip = view.world_to_clip;
    resolve_view.projection[0] = view.projection.columns[0].x;
    resolve_view.projection[1] = view.projection.columns[1].y;
    resolve_view.projection[2] = view.projection.columns[2].z;
    resolve_view.projection[3] = view.projection.columns[3].z;
    resolve_view.relative_to_view = view.relative_to_view;
    if (Status began = resolve_.begin_frame(resolve_view, frame); !began) {
        return began;
    }
    if (hardware_) {
        if (Status declared = structures_->declare(graph); !declared) {
            return declared;
        }
    }
    if (Status declared = declare_caches(graph); !declared) {
        return declared;
    }
    if (views_.created()) {
        if (Status prepared = views_.prepare(system, view.eye, frame, view_settings_); !prepared) {
            return prepared;
        }
    }
    out.diffuse = resolve_.import_diffuse(graph);
    out.specular = resolve_.import_specular(graph);
    if (views_.active()) {
        out.debug_view = views_.import_texture(graph);
    }
    return ok();
}

Status GpuGiFrame::declare_caches(RenderGraph& graph) noexcept {
    if (!desc_.device_caches) {
        return ok();
    }
    // WHAT THE HOST'S UPDATE SUBMITTED, run in this frame before the resolve reads the caches;
    // the next update applies what comes back. A submission is declared once.
    gi::CardGatherSettings card_gather = system_->settings().card_gather;
    card_gather.sky = system_->sky_term();
    shading_.set_gather(card_gather);
    const GpuSurfaceSubmission& shaded = shading_.last_submission();
    const GpuProbeSubmission& gathered = cache_.last_submission();
    const bool shade = shaded.pages != 0 && shaded.frame != shading_declared_;
    const bool gather = gathered.probes != 0 && gathered.frame != gather_declared_;
    if (!shade && !gather) {
        // Nothing ran on the caches this frame: the last stamps are an older frame's.
        timed_shading_ = false;
        timed_gather_ = false;
    }
    if (timer_ && (shade || gather)) {
        declare_stamp(graph, 0);
    }
    if (shade) {
        if (Status declared = shading_.declare(graph); !declared) {
            return declared;
        }
        shading_declared_ = shaded.frame;
    }
    if (timer_ && (shade || gather)) {
        declare_stamp(graph, 1);
    }
    if (gather) {
        if (Status declared = cache_.declare(graph); !declared) {
            return declared;
        }
        gather_declared_ = gathered.frame;
    }
    if (timer_ && (shade || gather)) {
        declare_stamp(graph, 2);
        timed_shading_ = shade;
        timed_gather_ = gather;
    }
    return ok();
}

void GpuGiFrame::declare_stamp(RenderGraph& graph, u32 index) noexcept {
    stamps_[index] = Stamp{this, index};
    // A pass with no resource is a root only by its side effect: the graph would cull it.
    graph.add_pass("gi caches timestamp", rhi::QueueKind::Graphics)
        .side_effect()
        .record(&record_stamp, &stamps_[index]);
}

void GpuGiFrame::record_stamp(const PassContext& context, void* user) noexcept {
    const auto* stamp = static_cast<const Stamp*>(user);
    GpuGiFrame& self = *stamp->self;
    if (stamp->index == 0) {
        context.commands->reset_queries(self.timer_, 0, 3);
    }
    context.commands->write_timestamp(self.timer_, stamp->index);
    self.timer_written_ = self.timer_written_ || stamp->index == 2;
}

GpuGiCost GpuGiFrame::last_cost() const noexcept {
    GpuGiCost cost;
    const GpuResolveTimes times = resolve_.last_times();
    cost.resolve_ms = times.total_ms;
    cost.resolve_valid = times.valid;
    if (device_ == nullptr || !timer_ || !timer_written_) {
        return cost;
    }
    u64 stamps[3] = {};
    Expected<u32, Error> read = device_->read_query_results(timer_, 0, 3, Span<u64>(stamps, 3));
    if (!read || *read != 3 || stamps[1] < stamps[0] || stamps[2] < stamps[1]) {
        return cost;
    }
    cost.cards_ms = static_cast<f64>(stamps[1] - stamps[0]) / 1.0e6;
    cost.cards_valid = timed_shading_;
    cost.gather_ms = static_cast<f64>(stamps[2] - stamps[1]) / 1.0e6;
    cost.gather_valid = timed_gather_;
    return cost;
}

FrameStageDeclaration GpuGiFrame::stage() noexcept {
    return FrameStageDeclaration{&declare_stage, this};
}

PassId GpuGiFrame::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                                 void* user) noexcept {
    auto& self = *static_cast<GpuGiFrame*>(user);
    const PassId first = self.resolve_.declare(graph, inputs);
    if (first == kInvalidPass) {
        return first;
    }
    if (Status declared = self.views_.declare(graph); !declared) {
        return kInvalidPass;
    }
    return first;
}

void GpuGiFrame::set_jitter(Vec2 jitter) noexcept {
    resolve_.set_jitter(jitter);
}

bool GpuGiFrame::composites() const noexcept {
    return system_ != nullptr && gi_admission(system_->runtime_settings().mode) != 0U;
}

void GpuGiFrame::write_control(u32 diffuse_slot, u32 specular_slot, u32 out[4]) const noexcept {
    if (system_ == nullptr) {
        return;
    }
    write_gi_control(diffuse_slot, specular_slot, system_->runtime_settings().mode, out);
}

}  // namespace cy::rendering::gi_gpu
