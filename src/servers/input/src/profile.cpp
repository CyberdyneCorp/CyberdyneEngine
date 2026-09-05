// Rebinding profiles and the rebinding flow. Task 4.1.5.

#include <cy/servers/input/profile.h>

namespace cy::input {

const char* conflict_policy_name(ConflictPolicy policy) noexcept {
    switch (policy) {
        case ConflictPolicy::Reject:
            return "Reject";
        case ConflictPolicy::Swap:
            return "Swap";
        case ConflictPolicy::Allow:
            return "Allow";
        case ConflictPolicy::UnbindPrevious:
            return "UnbindPrevious";
        case ConflictPolicy::Ask:
            return "Ask";
    }
    return "Reject";
}

const char* rebind_status_name(RebindStatus status) noexcept {
    switch (status) {
        case RebindStatus::Listening:
            return "Listening";
        case RebindStatus::Applied:
            return "Applied";
        case RebindStatus::Conflict:
            return "Conflict";
        case RebindStatus::Cancelled:
            return "Cancelled";
    }
    return "Listening";
}

Status BindingProfile::set(const BindingOverride& override_entry) noexcept {
    if (!override_entry.action.valid()) {
        return fail(ErrorCode::InvalidArgument, "input: an override names an action by stable id");
    }
    for (auto& existing : overrides_) {
        if (existing.action == override_entry.action && existing.scheme == override_entry.scheme &&
            existing.slot == override_entry.slot) {
            existing.control = override_entry.control;
            return ok();
        }
    }
    return overrides_.push_back(override_entry);
}

void BindingProfile::clear(ActionStableId action, SchemeKind scheme, u8 slot) noexcept {
    for (usize index = 0; index < overrides_.size(); ++index) {
        const BindingOverride& existing = overrides_[index];
        if (existing.action == action && existing.scheme == scheme && existing.slot == slot) {
            overrides_.erase(index);
            return;
        }
    }
}

const BindingOverride* BindingProfile::find(ActionStableId action, SchemeKind scheme,
                                            u8 slot) const noexcept {
    for (const auto& existing : overrides_) {
        if (existing.action == action && existing.scheme == scheme && existing.slot == slot) {
            return &existing;
        }
    }
    return nullptr;
}

const BindingOverride* BindingProfile::holder_of(Control control,
                                                 SchemeKind scheme) const noexcept {
    for (const auto& existing : overrides_) {
        if (existing.scheme == scheme && existing.control == control) {
            return &existing;
        }
    }
    return nullptr;
}

RebindStatus RebindOperation::offer(const DeviceEvent& event,
                                    const BindingProfile& profile) noexcept {
    if (status_ != RebindStatus::Listening) {
        return status_;
    }
    // A release is not a choice: a player who is still holding the key they pressed to open the
    // rebinding dialogue would otherwise bind it on the way up.
    if (event.value == 0.0F) {
        return status_;
    }
    if (event.control == cancel_control_ && cancel_control_.is_valid()) {
        status_ = RebindStatus::Cancelled;
        return status_;
    }
    // Controls from another scheme are ignored rather than refused — see the header. Overrides are
    // per scheme, so a gamepad button cannot become a keyboard override even by accident.
    if (scheme_of(event.control.kind) != scheme_) {
        return status_;
    }
    candidate_ = event.control;

    if (const BindingOverride* holder = profile.holder_of(candidate_, scheme_);
        holder != nullptr && holder->action != action_) {
        conflict_action_ = holder->action;
        conflict_slot_ = holder->slot;
        status_ = ConflictPolicy::Allow == policy_ ? RebindStatus::Applied : RebindStatus::Conflict;
        return status_;
    }
    status_ = RebindStatus::Applied;
    return status_;
}

Status RebindOperation::apply(BindingProfile& profile) noexcept {
    if (status_ != RebindStatus::Applied && status_ != RebindStatus::Conflict) {
        return fail(ErrorCode::Unavailable,
                    "input: the rebind has not produced a control to apply yet");
    }
    if (status_ == RebindStatus::Conflict) {
        switch (policy_) {
            case ConflictPolicy::Reject:
                return fail(ErrorCode::AlreadyExists,
                            "input: that control is already bound and the policy is Reject");
            case ConflictPolicy::Ask:
                // The interface has to decide. Leaving the operation open is the point: a policy
                // that says "ask" and then picks for the player is not asking.
                return fail(ErrorCode::Unavailable,
                            "input: the policy is Ask; resolve the conflict and apply again");
            case ConflictPolicy::Swap: {
                // The other action takes whatever this one had, which may be nothing. `find` before
                // `set`: the profile is about to be written and the previous value would be gone.
                const BindingOverride* mine = profile.find(action_, scheme_, slot_);
                const Control previous = mine != nullptr ? mine->control : Control{};
                if (Status swapped = profile.set(
                        BindingOverride{conflict_action_, scheme_, conflict_slot_, previous});
                    !swapped) {
                    return swapped;
                }
                break;
            }
            case ConflictPolicy::UnbindPrevious:
                if (Status unbound = profile.set(
                        BindingOverride{conflict_action_, scheme_, conflict_slot_, Control{}});
                    !unbound) {
                    return unbound;
                }
                break;
            case ConflictPolicy::Allow:
                break;
        }
    }
    return profile.set(BindingOverride{action_, scheme_, slot_, candidate_});
}

}  // namespace cy::input
