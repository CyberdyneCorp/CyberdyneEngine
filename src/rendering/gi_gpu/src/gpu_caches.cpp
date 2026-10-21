// SPDX-License-Identifier: MIT
#include <cy/rendering/gi_gpu/gpu_caches.h>

namespace cy::rendering::gi_gpu {

GpuGiCaches::~GpuGiCaches() {
    destroy();
}

Status GpuGiCaches::create(rhi::Device& device, GpuGiScene& scene, GpuRadianceCache& radiance,
                           gi::IlluminationSystem& system) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device GI caches are already created");
    }
    device_ = &device;
    system_ = &system;
    radiance_ = &radiance;
    shading_.bind(scene, system.field(), system.surfaces().lookup_radius());
    system.set_surface_shading(&shading_);
    system.set_probe_gather(&radiance);
    if (device.capabilities().has(rhi::Capability::TimestampQueries)) {
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

void GpuGiCaches::destroy() noexcept {
    if (system_ != nullptr) {
        system_->set_surface_shading(nullptr);
        system_->set_probe_gather(nullptr);
    }
    if (device_ != nullptr && timer_) {
        device_->destroy_query_pool(timer_);
    }
    timer_ = {};
    timer_written_ = false;
    timed_shading_ = false;
    timed_gather_ = false;
    shading_declared_ = ~u64{0};
    gather_declared_ = ~u64{0};
    system_ = nullptr;
    radiance_ = nullptr;
    device_ = nullptr;
}

Status GpuGiCaches::declare(RenderGraph& graph) noexcept {
    if (device_ == nullptr || system_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device GI caches are not created");
    }
    // WHAT THE HOST'S UPDATE SUBMITTED, run in this frame before the resolve reads the caches;
    // the next update applies what comes back. A submission is declared once.
    gi::CardGatherSettings card_gather = system_->settings().card_gather;
    card_gather.sky = system_->sky_term();
    shading_.set_gather(card_gather);
    const GpuSurfaceSubmission& shaded = shading_.last_submission();
    const GpuProbeSubmission& gathered = radiance_->last_submission();
    const bool shade = shaded.pages != 0 && shaded.frame != shading_declared_;
    const bool gather = gathered.probes != 0 && gathered.frame != gather_declared_;
    if (!shade && !gather) {
        // Nothing ran on the caches this frame: the last stamps are an older frame's.
        timed_shading_ = false;
        timed_gather_ = false;
        return ok();
    }
    if (timer_) {
        declare_stamp(graph, 0);
    }
    if (shade) {
        if (Status declared = shading_.declare(graph); !declared) {
            return declared;
        }
        shading_declared_ = shaded.frame;
    }
    if (timer_) {
        declare_stamp(graph, 1);
    }
    if (gather) {
        if (Status declared = radiance_->declare(graph); !declared) {
            return declared;
        }
        gather_declared_ = gathered.frame;
    }
    if (timer_) {
        declare_stamp(graph, 2);
        timed_shading_ = shade;
        timed_gather_ = gather;
    }
    return ok();
}

void GpuGiCaches::declare_stamp(RenderGraph& graph, u32 index) noexcept {
    stamps_[index] = Stamp{this, index};
    // A pass with no resource is a root only by its side effect: the graph would cull it.
    graph.add_pass("gi caches timestamp", rhi::QueueKind::Graphics)
        .side_effect()
        .record(&record_stamp, &stamps_[index]);
}

void GpuGiCaches::record_stamp(const PassContext& context, void* user) noexcept {
    const auto* stamp = static_cast<const Stamp*>(user);
    GpuGiCaches& self = *stamp->self;
    if (stamp->index == 0) {
        context.commands->reset_queries(self.timer_, 0, 3);
    }
    context.commands->write_timestamp(self.timer_, stamp->index);
    self.timer_written_ = self.timer_written_ || stamp->index == 2;
}

GpuGiCost GpuGiCaches::cost(GpuGiCost base) const noexcept {
    GpuGiCost cost = base;
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

}  // namespace cy::rendering::gi_gpu
