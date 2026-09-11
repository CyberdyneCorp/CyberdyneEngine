// The camera bridge. Four calls into `cy::camera`, and no fifth — see bridge.h.

#include <cy/sequencing/camera/bridge.h>

namespace cy::sequencing {

cy::camera::BlendCurve blend_curve_of(u8 curve) noexcept {
    switch (curve) {
        case 0:
            return cy::camera::BlendCurve::Linear;
        case 1:
            return cy::camera::BlendCurve::EaseIn;
        case 2:
            return cy::camera::BlendCurve::EaseOut;
        case 4:
            return cy::camera::BlendCurve::Step;
        default:
            break;
    }
    return cy::camera::BlendCurve::EaseInOut;
}

namespace {

[[nodiscard]] cy::camera::BlendPolicy policy_of(const CameraBlend& blend) noexcept {
    cy::camera::BlendPolicy policy;
    policy.duration_seconds = blend.duration_seconds;
    policy.curve = blend_curve_of(blend.curve);
    policy.position = blend.position;
    policy.rotation = blend.rotation;
    policy.lens = blend.lens;
    return policy;
}

}  // namespace

CameraStackBridge::CameraStackBridge(Allocator& allocator,
                                     cy::camera::CameraServer& server) noexcept
    : server_(&server), rigs_(allocator), entries_(allocator) {}

Status CameraStackBridge::initialize(cy::camera::StackHandle stack) noexcept {
    if (server_->stack(stack) == nullptr) {
        return fail(ErrorCode::InvalidArgument, "no such camera stack");
    }
    stack_ = stack;
    entries_.clear();
    return ok();
}

Status CameraStackBridge::bind_rig(u64 identity, cy::camera::RigHandle rig) noexcept {
    if (identity == 0) {
        return fail(ErrorCode::InvalidArgument, "a rig identity of zero is the unresolved binding");
    }
    for (RigBinding& binding : rigs_.span()) {
        if (binding.identity == identity) {
            binding.rig = rig;
            return ok();
        }
    }
    return rigs_.push_back(RigBinding{identity, rig});
}

cy::camera::RigHandle CameraStackBridge::rig_for(u64 identity) const noexcept {
    for (const RigBinding& binding : rigs_.span()) {
        if (binding.identity == identity) {
            return binding.rig;
        }
    }
    return cy::camera::RigHandle{};
}

BridgeEntry* CameraStackBridge::entry_for(u32 binding) noexcept {
    for (BridgeEntry& entry : entries_.span()) {
        if (entry.binding == binding) {
            return &entry;
        }
    }
    return nullptr;
}

Status CameraStackBridge::push_shot(const CameraRequest& request, cy::camera::RigHandle rig,
                                    CameraBridgeReport& report) noexcept {
    cy::camera::CameraStack* stack = server_->stack(stack_);
    if (stack == nullptr) {
        return fail(ErrorCode::Unavailable, "the bridge has no stack");
    }
    cy::camera::StackEntry entry;
    entry.rig = rig;
    // A CINEMATIC CONTRIBUTION, declared as one. `camera-system`'s stack has a `Cinematic` kind
    // precisely so that "the camera is not where I expect" is answered by a contribution report
    // naming a cinematic rather than by a pose nobody can attribute.
    entry.kind = cy::camera::ContributionKind::Cinematic;
    entry.priority = request.priority;
    entry.target_weight = request.weight;
    entry.blend_in = policy_of(request.blend_in);
    entry.blend_out = policy_of(request.blend_out);

    const Expected<cy::camera::StackEntryId, Error> pushed = stack->push(entry);
    if (!pushed) {
        return Status{make_unexpected(pushed.error())};
    }
    BridgeEntry record;
    record.binding = request.binding;
    record.rig_identity = request.rig;
    record.rig = rig;
    record.stack_entry = pushed.value();
    if (Status kept = entries_.push_back(record); !kept) {
        return kept;
    }
    ++report.pushed;
    return ok();
}

Status CameraStackBridge::release_shot(const CameraRequest& request,
                                       CameraBridgeReport& report) noexcept {
    cy::camera::CameraStack* stack = server_->stack(stack_);
    if (stack == nullptr) {
        return fail(ErrorCode::Unavailable, "the bridge has no stack");
    }
    for (usize index = 0; index < entries_.size(); ++index) {
        if (entries_[index].binding != request.binding) {
            continue;
        }
        // RELEASED, NOT REMOVED. The entry blends out over its declared policy, which is what makes
        // "a cinematic takes over and later releases" a blend in both directions rather than a snap
        // back to the gameplay camera.
        if (Status released = stack->release(entries_[index].stack_entry); !released) {
            return released;
        }
        entries_.remove_unordered(index);
        ++report.released;
        return ok();
    }
    return ok();
}

Status CameraStackBridge::apply(Span<const CameraRequest> requests,
                                CameraBridgeReport& report) noexcept {
    // Cuts first: a cut is raised on the RIG and is independent of which contributions are in the
    // stack, so it must not be affected by the pass order below.
    for (const CameraRequest& request : requests) {
        if (!request.cut) {
            continue;
        }
        const cy::camera::RigHandle rig = rig_for(request.rig);
        if (!server_->alive(rig)) {
            ++report.unresolved_rigs;
            continue;
        }
        // A cut invalidates the temporal history of every view derived from this rig, and an
        // anticipated one puts a deadline on its streaming source. Both are the camera's own doing;
        // the sequence only says when.
        if (Status cut = server_->cut(rig, cy::camera::CutReason::CinematicStart,
                                      request.anticipated, request.cut_lead_seconds);
            !cut) {
            return cut;
        }
        report.cuts += request.anticipated ? 0U : 1U;
        report.anticipated_cuts += request.anticipated ? 1U : 0U;
    }

    // THE LIVE PASS, then the release pass — and a live request whose binding AND rig are also
    // being released is skipped rather than pushed.
    //
    // The reason is a frame this milestone's capture actually produced: a sequence's last frame
    // both evaluates its final shot and completes, so the batch carries the shot and its release
    // together. Pushing then releasing would leave a contribution blending out from nothing;
    // releasing then pushing would leave one that never goes away. A shot CHANGE on one binding is
    // the case that keeps this from being "releases win": there the released rig and the live rig
    // differ, and both have to happen.
    for (const CameraRequest& request : requests) {
        if (request.cut || request.release) {
            continue;
        }
        bool superseded = false;
        for (const CameraRequest& other : requests) {
            superseded = superseded || (other.release && other.binding == request.binding &&
                                        other.rig == request.rig);
        }
        if (superseded) {
            continue;
        }
        const cy::camera::RigHandle rig = rig_for(request.rig);
        if (!server_->alive(rig)) {
            ++report.unresolved_rigs;
            continue;
        }

        BridgeEntry* existing = entry_for(request.binding);
        if (existing != nullptr && existing->rig_identity != request.rig) {
            // The shot changed rig: release the old contribution so it blends out, and push the new
            // one so it blends in. Both are in the stack for the duration of the blend, which is
            // exactly what a cut between two shots looks like.
            if (Status released = release_shot(request, report); !released) {
                return released;
            }
            existing = nullptr;
        }
        if (existing == nullptr) {
            if (Status pushed = push_shot(request, rig, report); !pushed) {
                return pushed;
            }
            existing = entry_for(request.binding);
        }

        if (request.has_framing_target && request.framing_target != 0 && existing != nullptr &&
            existing->framing_target != request.framing_target) {
            cy::camera::TargetBinding target;
            target.kind = cy::camera::TargetKind::Entity;
            target.stable_id = request.framing_target;
            if (Status bound = server_->set_target(rig, target); !bound) {
                // Refused by a rig with no `Target` node. Counted, because a shot that frames
                // nothing is a content mistake and not a bridge failure.
                ++report.framing_refused;
            } else {
                existing->framing_target = request.framing_target;
                ++report.framing_targets_set;
            }
        }
    }

    for (const CameraRequest& request : requests) {
        if (!request.release) {
            continue;
        }
        if (Status released = release_shot(request, report); !released) {
            return released;
        }
    }
    return ok();
}

}  // namespace cy::sequencing
