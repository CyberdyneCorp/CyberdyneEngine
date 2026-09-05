#pragma once
// The per-tick command frame, and the text stream. Task 4.1.4.
//
// `input-and-actions` — "Fixed-tick sampling": "For continuous control, the system SHALL produce a
// **command frame** per simulation tick — a compact record of the tick number, continuous axis
// values, and button state — suitable for prediction and replay. Command frames SHALL be the input
// side of the gameplay command stream defined in `gameplay-framework`, and discrete complex intents
// SHALL be typed commands rather than packed bits."
//
// That last clause is the design constraint, and it is why this record is small and fixed. A
// command frame is what a client sends sixty times a second and what a predictor re-applies when a
// correction arrives; it carries the *continuous* half of intent, where re-sending is cheaper than
// reliability. Everything discrete and complex — "build a turret here", "use ability three on that
// target" — is a typed gameplay command and does not appear in this struct. An engine that packed
// those into bits here would have made its network protocol its input format.
//
// TWO EDGE MASKS, NOT ONE STATE MASK. `pressed` is the level at the end of the tick;
// `just_pressed` and `just_released` are the transitions *within* it. A frame carrying only the
// level cannot express "pressed and released between two ticks", which is the whole subject of
// design.md §5 — and a replay reconstructed from level-only frames loses exactly the inputs that
// were hardest to make.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>

namespace cy::input {

/// How many digital actions a command frame carries. One 32-bit word per mask; a project needing
/// more declares the rest as typed commands, which is what the header comment says they should be
/// anyway.
inline constexpr u32 kMaxFrameButtons = 32;
/// How many continuous axes a command frame carries.
inline constexpr u32 kMaxFrameAxes = 8;

/// One tick's worth of continuous intent for one user.
struct CommandFrame {
    u64 tick = 0;
    /// The input user this frame belongs to. Not a device index — see `input-and-actions`: "An
    /// input user SHALL NOT be identified by a device index".
    u32 user = 0;
    /// Held at the end of the tick.
    u32 pressed = 0;
    /// Went down at least once during the tick, whether or not it is still down.
    u32 just_pressed = 0;
    /// Came up at least once during the tick.
    u32 just_released = 0;
    u8 axis_count = 0;
    /// Continuous values, in declaration order among the actions marked `in_command_frame`. A
    /// scalar action uses `x` and leaves `y` at zero.
    Vec2 axes[kMaxFrameAxes];

    /// A cheap value identity, for a replay's per-tick check and for a desync report. Not a
    /// cryptographic digest and not a substitute for `core-determinism`'s state hash — it answers
    /// "did the two peers feed the same input into this tick", which is the first question when a
    /// simulation diverges and the one that is usually skipped.
    [[nodiscard]] u64 hash() const noexcept;

    friend bool operator==(const CommandFrame& a, const CommandFrame& b) noexcept;
};

/// One unit of composed text.
///
/// `input-and-actions` — "Text entry is separate": text entry uses the platform's text input
/// services and "SHALL NOT be reconstructed by interpreting key actions or scan codes". Key actions
/// and text are distinct streams and this is the second one. A field consumes text; navigation
/// consumes actions; neither is built from the other.
///
/// The payload is UTF-8 and inline: a composed sequence from an input method editor is short, and a
/// pointer here would be a lifetime question at every hand-off.
struct TextEvent {
    Nanoseconds timestamp = 0;
    /// Which user's text stream. Text follows interface focus, not device assignment.
    u32 user = 0;
    /// UTF-8, not null-terminated. `length` is in bytes.
    char text[32] = {};
    u8 length = 0;
    /// True while an input method editor is composing and the text is provisional.
    bool composing = false;
};

}  // namespace cy::input
