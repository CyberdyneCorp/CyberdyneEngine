#include <cy/rendering/virtual_texturing/frame_residency.h>

namespace cy::rendering::vt {
namespace {

using residency::CostClass;
using residency::Lever;
using residency::LeverSchedule;
using residency::PageKey;
using residency::Request;
using residency::Subsystem;
using residency::SubsystemPolicy;

/// How many samples a page needs before it counts as fully important. The same shape
/// `VirtualTextureSystem` uses: importance saturates, because the difference between ten thousand
/// pixels and twenty thousand is not a residency decision.
constexpr f32 kImportanceSaturation = 256.0F;

[[nodiscard]] f32 importance_from_samples(u32 samples) noexcept {
    const f32 scaled = static_cast<f32>(samples) / kImportanceSaturation;
    return scaled > 1.0F ? 1.0F : scaled;
}

/// A geometry page's key: the instance slot in the high bits and the level in the low ones.
///
/// The pair, not the instance. A mesh at level 2 and the same mesh at level 0 are two different
/// resident things, which is what makes a geometry ladder a ladder rather than a boolean — and it
/// is what lets the policy evict the fine rung while keeping the coarse one, which is the whole of
/// "a frame is coarser, never missing".
[[nodiscard]] u64 geometry_page(u32 instance_slot, u32 level) noexcept {
    return (static_cast<u64>(instance_slot) << 8U) | (level & 0xFFU);
}

}  // namespace

// --- The two policies -------------------------------------------------------------------------

SubsystemPolicy texture_policy(const FrameResidencySettings& settings) noexcept {
    SubsystemPolicy policy;
    policy.domain = MemoryDomain::Gpu;
    policy.budget_bytes = settings.texture_budget_bytes;
    policy.budget_kind = BudgetKind::Hard;
    policy.reduction_order = settings.texture_reduction_order;
    policy.min_residency_frames = 2;
    policy.default_cost = CostClass::Streamed;

    // The declared ladder. `TextureMipBias` positive means coarser, and each position costs less
    // than the one before it because a mip is a quarter of the level above.
    LeverSchedule& mip_bias = policy.levers[static_cast<u32>(Lever::TextureMipBias)];
    mip_bias.declared = true;
    mip_bias.normal = 0.0F;
    mip_bias.elevated = 1.0F;
    mip_bias.critical = 2.0F;
    mip_bias.relative_cost[0] = 1.0F;
    mip_bias.relative_cost[1] = 0.55F;
    mip_bias.relative_cost[2] = 0.30F;

    LeverSchedule& prefetch = policy.levers[static_cast<u32>(Lever::TexturePrefetchRadius)];
    prefetch.declared = true;
    prefetch.normal = 3.0F;
    prefetch.elevated = 2.0F;
    prefetch.critical = 1.0F;
    prefetch.relative_cost[0] = 1.0F;
    prefetch.relative_cost[1] = 0.70F;
    prefetch.relative_cost[2] = 0.40F;
    return policy;
}

SubsystemPolicy geometry_policy(const FrameResidencySettings& settings) noexcept {
    SubsystemPolicy policy;
    policy.domain = MemoryDomain::Gpu;
    policy.budget_bytes = settings.geometry_budget_bytes;
    policy.budget_kind = BudgetKind::Hard;
    policy.reduction_order = settings.geometry_reduction_order;
    // Geometry pages are content-addressed and cheap to re-fetch, so a shorter minimum age costs
    // less here than it would for a rendered shadow page. `types.h` says exactly that about
    // `Subsystem::Geometry`.
    policy.min_residency_frames = 2;
    policy.default_cost = CostClass::Streamed;

    LeverSchedule& error = policy.levers[static_cast<u32>(Lever::GeometryErrorThreshold)];
    error.declared = true;
    error.normal = 1.0F;
    error.elevated = 2.0F;
    error.critical = 4.0F;
    // Doubling the permitted screen-space error roughly halves the triangles that survive, which is
    // what these numbers say. They are the ladder's PRICE, not its quality.
    error.relative_cost[0] = 1.0F;
    error.relative_cost[1] = 0.60F;
    error.relative_cost[2] = 0.35F;
    return policy;
}

// --- Registration -------------------------------------------------------------------------------

Status FrameResidency::register_subsystems(residency::ResidencyServer& server,
                                           const FrameResidencySettings& settings) noexcept {
    if (settings.texture_budget_bytes == 0 || settings.geometry_budget_bytes == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "both budgets must be non-zero: BudgetTree's zero means UNBUDGETED, and two "
                    "unbudgeted claimants would both be granted everything, which is not "
                    "arbitration");
    }
    if (settings.bytes_per_tile == 0 || settings.bytes_per_geometry_level == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "the policy cannot budget what it cannot size: bytes_per_tile and "
                    "bytes_per_geometry_level are what a request's `bytes` is filled from");
    }
    if (Status registered = server.register_subsystem(Subsystem::Texture, texture_policy(settings));
        !registered) {
        return registered;
    }
    if (Status registered =
            server.register_subsystem(Subsystem::Geometry, geometry_policy(settings));
        !registered) {
        return registered;
    }
    server_ = &server;
    settings_ = settings;
    return ok();
}

// --- The two feedback streams
// ---------------------------------------------------------------------

Status FrameResidency::submit_texture_feedback(Span<const FeedbackRequest> requests,
                                               f64 now) noexcept {
    if (server_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the frame residency has not registered");
    }
    (void)now;
    for (const FeedbackRequest& entry : requests) {
        const VirtualAddress address = VirtualAddress::decode(entry.address);
        Request request;
        request.key = PageKey{Subsystem::Texture, entry.address};
        request.bytes = settings_.bytes_per_tile;
        request.instance = address.texture;
        request.guaranteed = false;
        request.inputs.importance = importance_from_samples(entry.samples);
        request.inputs.screen_coverage = importance_from_samples(entry.samples);
        // The deficit is the mip's distance from the finest level: a coarse page that is missing
        // costs less than a fine one that is.
        request.inputs.detail_deficit = address.mip;
        // FEEDBACK IS A MEASUREMENT, NOT A PREDICTION. Confidence is 1 for the reason
        // `VirtualTextureSystem::submit_feedback_requests` gives: something sampled this page.
        request.inputs.prediction_confidence = 1.0F;
        request.inputs.cost = CostClass::Streamed;
        if (Status submitted = server_->request(request); !submitted) {
            return submitted;
        }
        ++texture_requests_;
    }
    return ok();
}

Status FrameResidency::submit_geometry_feedback(
    Span<const render::culling::GpuDrawPayload> payloads, u32 root_level, f64 now) noexcept {
    if (server_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the frame residency has not registered");
    }
    (void)now;
    for (const render::culling::GpuDrawPayload& payload : payloads) {
        Request request;
        request.key =
            PageKey{Subsystem::Geometry, geometry_page(payload.instance_slot, payload.lod_level)};
        request.bytes = settings_.bytes_per_geometry_level;
        request.instance = payload.instance_slot;
        // THE COARSEST LEVEL IS GUARANTEED. `residency`: "No residency system blocks another" holds
        // because a guaranteed set exists — a mip tail for a texture, and for geometry the root,
        // the level nothing may evict. A frame is coarser, never missing.
        request.guaranteed = payload.lod_level >= root_level;
        request.inputs.importance = payload.coverage > 1.0F ? 1.0F : payload.coverage;
        request.inputs.screen_coverage = payload.coverage > 1.0F ? 1.0F : payload.coverage;
        request.inputs.detail_deficit = payload.lod_level;
        request.inputs.prediction_confidence = 1.0F;
        request.inputs.cost = CostClass::Streamed;
        if (Status submitted = server_->request(request); !submitted) {
            return submitted;
        }
        ++geometry_requests_;
    }
    return ok();
}

// --- Arbitration
// ----------------------------------------------------------------------------------

Status FrameResidency::arbitrate(f64 now, u32 max_admissions,
                                 FrameResidencyReport& report) noexcept {
    if (server_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the frame residency has not registered");
    }
    residency::Schedule schedule;
    residency::ScheduleOptions options;
    options.now = now;
    options.max_admissions = max_admissions;
    if (Status scheduled = server_->schedule(options, schedule); !scheduled) {
        return scheduled;
    }

    // WHAT A REAL SUBSYSTEM WOULD DO HERE is fetch or produce the admitted page and then report it
    // resident. This module has no storage of its own — `residency` stores the record and not the
    // page — so it reports each admission resident immediately, which is what a cache with an
    // instant fetch would do and is exactly what makes the budget arithmetic observable in one
    // frame. A real fetch reports later; the policy does not care which, and that is the point of
    // `ResidentPage::pending`.
    for (const residency::Admission& admission : schedule.admissions) {
        residency::ResidentReport resident;
        resident.key = admission.key;
        resident.bytes = admission.bytes;
        resident.level = static_cast<u32>(admission.key.page & 0xFFU);
        resident.cost = CostClass::Streamed;
        if (Status noted = server_->note_resident(resident, now); !noted) {
            return noted;
        }
    }

    report.texture_requests = texture_requests_;
    report.geometry_requests = geometry_requests_;
    report.admissions = static_cast<u32>(schedule.admissions.size());
    report.evictions = static_cast<u32>(schedule.evictions.size());
    report.texture_resident_bytes = server_->resident_bytes(Subsystem::Texture);
    report.geometry_resident_bytes = server_->resident_bytes(Subsystem::Geometry);

    Array<residency::ReductionStep> plan(current_allocator());
    if (Status read = server_->last_reduction(plan); read) {
        report.reduction_steps = static_cast<u32>(plan.size());
        report.first_reduced = plan.empty() ? residency::Subsystem::Count : plan[0].subsystem;
    }

    texture_requests_ = 0;
    geometry_requests_ = 0;
    server_->end_frame(now);
    return ok();
}

Status FrameResidency::reduction_plan(Array<residency::ReductionStep>& out) const noexcept {
    if (server_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the frame residency has not registered");
    }
    return server_->last_reduction(out);
}

}  // namespace cy::rendering::vt
