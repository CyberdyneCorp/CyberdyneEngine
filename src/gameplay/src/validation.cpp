// Validation results and their reasons. Task 4.4.4.

#include <cy/gameplay/validation.h>

namespace cy::gameplay {

const char* reason_tag_name(ReasonTag tag) noexcept {
    switch (tag) {
        case ReasonTag::None:
            return "None";
        case ReasonTag::UnknownCommand:
            return "UnknownCommand";
        case ReasonTag::NoSuchParticipant:
            return "NoSuchParticipant";
        case ReasonTag::NoSuchSource:
            return "NoSuchSource";
        case ReasonTag::NotControlled:
            return "NotControlled";
        case ReasonTag::CapabilityMissing:
            return "CapabilityMissing";
        case ReasonTag::TargetInvalid:
            return "TargetInvalid";
        case ReasonTag::WrongPhase:
            return "WrongPhase";
        case ReasonTag::OutOfRange:
            return "OutOfRange";
        case ReasonTag::InsufficientResource:
            return "InsufficientResource";
        case ReasonTag::Cooldown:
            return "Cooldown";
        case ReasonTag::ProjectDefined:
            return "ProjectDefined";
        case ReasonTag::Count:
            break;
    }
    return "None";
}

bool ValidationResult::has(ReasonTag tag) const noexcept {
    for (u32 index = 0; index < count_; ++index) {
        if (reasons_[index].tag == tag) {
            return true;
        }
    }
    return false;
}

void ValidationResult::reject(const ValidationReason& reason) noexcept {
    if (count_ == kMaxReasons) {
        // The reason is dropped and the fact is recorded. Replacing an earlier reason would lose
        // the *first* one, which is the one an interface shows; and quietly succeeding would make a
        // fifth rejection look like a permission.
        overflowed_ = true;
        return;
    }
    reasons_[count_++] = reason;
}

}  // namespace cy::gameplay
