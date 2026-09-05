#pragma once
// Command validation returns reasons. Task 4.4.4.
//
// `gameplay-framework` — "Command validation returns reasons": validation returns a **structured
// result** — whether the command is permitted, and when it is not, tagged reasons with the data
// behind them. "`true` or `false` SHALL NOT be the validation interface."
//
// ================================================================================================
// WHY A BOOL IS THE WRONG RETURN TYPE, STATED ONCE
// ================================================================================================
//
// Not because a bool is unhelpful — because a bool forces the four consumers to disagree.
//
//   * the interface wants to grey out a button and say "you need 40 more ore";
//   * an artificial intelligence wants to know *which* precondition failed so it can fix it;
//   * the authority wants to reject the command;
//   * a test wants to assert on the reason rather than on the outcome.
//
// With a bool, three of those four grow their own re-implementation of the rule and the four drift
// apart — and the day they disagree is the day a client shows an action as available that the
// server rejects. One result type carrying tagged reasons is the whole fix, and it is why
// `ValidationResult` is a value with room for several reasons rather than an error code.
//
// A REASON HAS DATA. "You cannot build that" is unactionable; `InsufficientResource{ore, required
// 100, available 60}` renders as a sentence, drives an AI's next decision and asserts in a test.
// The tag is an enumerator so it can be switched on and searched for; the numbers ride along.

#include <cy/core/base/types.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>

namespace cy::gameplay {

/// Why a command was refused. The structural reasons come first, in the order the engine checks
/// them, because that order is itself a requirement: "The engine SHALL validate structurally before
/// game-specific logic runs."
enum class ReasonTag : u16 {
    None = 0,
    /// The command type was never declared.
    UnknownCommand,
    /// The participant does not exist in this session.
    NoSuchParticipant,
    /// The control source does not exist.
    NoSuchSource,
    /// The source does not control the target on the command's channel.
    NotControlled,
    /// The target does not accept the capability this command requires.
    CapabilityMissing,
    /// The target entity is not valid.
    TargetInvalid,
    /// The session is not in a phase that permits this.
    WrongPhase,
    /// Game-specific, and the ones a rule usually reaches for.
    OutOfRange,
    InsufficientResource,
    Cooldown,
    /// A project's own reason; `detail` names it.
    ProjectDefined,
    Count,
};

const char* reason_tag_name(ReasonTag tag) noexcept;

/// One reason, with the data behind it.
struct ValidationReason {
    ReasonTag tag = ReasonTag::None;
    /// What the reason is about: the resource, the ability, the project-defined reason's own name.
    Name detail;
    /// The numbers, for the two cases that always have them. Left at zero where they mean nothing —
    /// a reason with no quantity is not a reason with a quantity of zero, and `detail` says which.
    f32 required = 0.0F;
    f32 available = 0.0F;
    /// The entity the reason is about, when it is not the command's own target.
    ecs::Entity subject;
};

/// The result of validating one command.
///
/// A value: copyable, allocation-free, and small enough to return. The reason capacity is fixed
/// because a command refused for more than four reasons is a command whose first reason is the
/// answer — and an unbounded list here would put an allocation on the path an interface calls for
/// every button, every frame.
class ValidationResult {
public:
    static constexpr u32 kMaxReasons = 4;

    /// Permitted until something says otherwise. The default is the permissive one because every
    /// check below adds a reason on failure; a default-rejected result would need a "permit"
    /// call that somebody would forget, and forgetting it would deny everything rather than
    /// allowing everything — safe, but it would make every new command type silently broken.
    ValidationResult() = default;

    [[nodiscard]] bool permitted() const noexcept { return count_ == 0; }
    [[nodiscard]] u32 reason_count() const noexcept { return count_; }
    [[nodiscard]] const ValidationReason& reason(u32 index) const noexcept {
        return reasons_[index];
    }

    /// The first reason, or a `None` one. What an interface prints when it has room for a sentence.
    [[nodiscard]] const ValidationReason& first() const noexcept { return reasons_[0]; }

    [[nodiscard]] bool has(ReasonTag tag) const noexcept;

    /// Add a reason. Beyond `kMaxReasons` the reason is dropped and `overflowed()` says so, rather
    /// than the result silently becoming permitted or the earlier reasons being replaced.
    void reject(const ValidationReason& reason) noexcept;
    void reject(ReasonTag tag) noexcept { reject(ValidationReason{tag, Name{}, 0.0F, 0.0F, {}}); }
    void reject(ReasonTag tag, Name detail, f32 required, f32 available) noexcept {
        reject(ValidationReason{tag, detail, required, available, {}});
    }

    [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }

private:
    ValidationReason reasons_[kMaxReasons];
    u32 count_ = 0;
    bool overflowed_ = false;
};

}  // namespace cy::gameplay
