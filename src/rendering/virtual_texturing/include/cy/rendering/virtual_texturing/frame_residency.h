#pragma once
// Two subsystems competing for one budget, driven by one frame's own GPU feedback. M7 task 4.3.
//
// `residency` — "Budgets and pressure response": "On rising memory pressure the layer SHALL apply a
// **coordinated reduction** across subsystems weighted by importance and by visible impact, rather
// than each subsystem independently evicting — which produces one subsystem freeing memory another
// immediately consumes."
//
// ================================================================================================
// WHY THIS FILE EXISTS AT ALL
// ================================================================================================
//
// The whole of `residency` is about arbitration BETWEEN subsystems, and until M7 exactly one thing
// in this tree registered one: `samples/06-open-world` registers `Subsystem::Texture` and its own
// comment says so — "One subsystem, because this sample pages one kind of thing; the point of
// `residency` is that a second one would join the same policy." A capability whose entire subject
// is arbitration, exercised by a single claimant, is a capability nothing has tested.
//
// So this is the second claimant, and it is not a second claimant invented to have one. Both are
// fed by the SAME FRAME'S GPU feedback:
//
//   `Subsystem::Texture`   — the compacted page requests `VirtualTextureFrame`'s resolve pass
//                            produced. One entry per page, with how many samples asked for it.
//   `Subsystem::Geometry`  — the level each instance was actually drawn at, which is what the
//                            culling dispatch's payloads say. A mesh LOD is a paged resource with a
//                            ladder exactly as a texture mip is, and the renderer already knows
//                            which rung it used because the cull chose it.
//
// Neither is a simulation of feedback. Both arrive from a dispatch that ran.
//
// ================================================================================================
// THE TWO POLICIES ARE DIFFERENT, AND THE DIFFERENCE IS THE POINT
// ================================================================================================
//
// `SubsystemPolicy::reduction_order` is "lower reduces first", and the two defaults here are 0 for
// texture and 1 for geometry — because a coarser texture mip is less visible than a coarser mesh,
// and `residency` requires the order to be DECLARED rather than emergent. `plan_reduction` walks
// it, and `reduction_plan()` below is how a caller sees which subsystem gave way first.
//
// The two also declare different levers — `TextureMipBias` and `GeometryErrorThreshold` — and each
// declares what its ladder positions COST relative to position 0, which is the field M7's arbiter
// spike found `LeverSchedule` was missing (design.md §2.10).

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/virtual_texturing/frame.h>
#include <cy/servers/render/culling/gpu_cull.h>
#include <cy/servers/residency/policy.h>
#include <cy/servers/residency/server.h>

namespace cy::rendering::vt {

/// What each subsystem is given and how it degrades. Two policies rather than one shared default,
/// because two subsystems with identical policies would arbitrate but would not demonstrate that
/// the declared order decides anything.
struct FrameResidencySettings {
    /// The texture cache's budget, in bytes.
    u64 texture_budget_bytes = 0;
    /// The geometry page budget, in bytes.
    u64 geometry_budget_bytes = 0;
    /// Bytes one virtual texture tile occupies, cooked. `VirtualTextureDesc::bytes_per_tile`.
    u32 bytes_per_tile = 0;
    /// Bytes one mesh LOD level occupies. A per-level number rather than a per-mesh one, because
    /// the ladder is the levels and the budget is spent a rung at a time.
    u32 bytes_per_geometry_level = 0;
    /// Lower reduces first. Texture before geometry by default: a coarser mip is less visible than
    /// a coarser mesh.
    u32 texture_reduction_order = 0;
    u32 geometry_reduction_order = 1;
};

/// How the last frame's arbitration went, per subsystem.
struct FrameResidencyReport {
    u32 texture_requests = 0;
    u32 geometry_requests = 0;
    u32 admissions = 0;
    u32 evictions = 0;
    u64 texture_resident_bytes = 0;
    u64 geometry_resident_bytes = 0;
    /// How many reduction steps the last pressure response produced, and which subsystem the first
    /// one belonged to. `residency` requires a coordinated reduction "in a declared order", and
    /// this is that order observed rather than asserted.
    u32 reduction_steps = 0;
    residency::Subsystem first_reduced = residency::Subsystem::Count;
};

/// The join: two subsystems, one residency server, one frame's feedback.
///
/// It owns no storage. `residency` stores the RECORD of a page and not the page — the distinction
/// the whole capability rests on — and this class is on the same side of that line: it turns a
/// frame's two feedback streams into requests, asks the server for a schedule, and reports what the
/// server said.
class FrameResidency {
public:
    FrameResidency() = default;

    FrameResidency(const FrameResidency&) = delete;
    FrameResidency& operator=(const FrameResidency&) = delete;
    FrameResidency(FrameResidency&&) = delete;
    FrameResidency& operator=(FrameResidency&&) = delete;

    /// Register both subsystems against `server`. Fails naming the field when a budget is zero,
    /// because an unbudgeted subsystem does not arbitrate — `BudgetTree`'s zero means unbudgeted,
    /// and two unbudgeted claimants would both be granted everything and demonstrate nothing.
    [[nodiscard]] Status register_subsystems(residency::ResidencyServer& server,
                                             const FrameResidencySettings& settings) noexcept;

    /// Turn the resolve pass's compacted page requests into texture residency requests.
    ///
    /// The sample count is the priority input, exactly as `VirtualTextureSystem::request_page` uses
    /// it: importance and screen contribution both come from how many pixels asked.
    [[nodiscard]] Status submit_texture_feedback(Span<const FeedbackRequest> requests,
                                                 f64 now) noexcept;

    /// Turn the culling dispatch's payloads into geometry residency requests.
    ///
    /// One request per (instance, level) actually drawn. The page key is the pair, because a mesh
    /// at level 2 and the same mesh at level 0 are two different resident things — which is what
    /// makes a geometry ladder a ladder rather than a boolean.
    ///
    /// `root_level` is the rung that is GUARANTEED resident — virtual geometry's always-resident
    /// root, the counterpart of a texture's mip tail. `residency`'s "No residency system blocks
    /// another" holds because such a set exists, and a request that did not carry the flag would
    /// leave the coarsest geometry evictable, which is how a frame becomes missing rather than
    /// coarse. The coarsest level of the chain is the usual answer.
    [[nodiscard]] Status submit_geometry_feedback(
        Span<const render::culling::GpuDrawPayload> payloads, u32 root_level, f64 now) noexcept;

    /// Ask the server to arbitrate, apply what it said, and end the frame.
    ///
    /// `max_admissions` is the frame's own budget for STARTING work, which is a different quantity
    /// from the memory budget and is why `ScheduleOptions` carries it separately.
    [[nodiscard]] Status arbitrate(f64 now, u32 max_admissions,
                                   FrameResidencyReport& report) noexcept;

    /// The reduction plan the last pressure response produced, in declared order.
    [[nodiscard]] Status reduction_plan(Array<residency::ReductionStep>& out) const noexcept;

    [[nodiscard]] bool registered() const noexcept { return server_ != nullptr; }

private:
    residency::ResidencyServer* server_ = nullptr;
    FrameResidencySettings settings_{};
    u32 texture_requests_ = 0;
    u32 geometry_requests_ = 0;
};

/// The two policies, published as functions so a caller can inspect or override one before
/// registering. `residency` wants the declared order and the declared levers to be visible; a
/// policy built inside a constructor is a policy nobody can read.
[[nodiscard]] residency::SubsystemPolicy texture_policy(
    const FrameResidencySettings& settings) noexcept;
[[nodiscard]] residency::SubsystemPolicy geometry_policy(
    const FrameResidencySettings& settings) noexcept;

}  // namespace cy::rendering::vt
