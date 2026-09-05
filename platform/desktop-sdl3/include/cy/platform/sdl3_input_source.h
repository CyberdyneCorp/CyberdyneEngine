#pragma once
// The SDL3 input event source: the platform end of `input-and-actions`' boundary. Task 4.1.1.
//
// `input-and-actions` — "The platform boundary": the platform layer produces normalised,
// **timestamped** device events carrying "a high-resolution timestamp, a stable device identifier,
// and a control identifier, so that latency analysis, fixed-tick resolution, and replay are
// possible". Everything above translates them; nothing above sees SDL.
//
// ================================================================================================
// WHY THIS IS AN EVENT WATCH AND NOT A SECOND PUMP
// ================================================================================================
//
// `Sdl3DisplayServer::pump_events()` already drains SDL's queue with `SDL_PollEvent` and discards
// everything that is not a window event. A second `SDL_PollEvent` loop here would not see those
// events at all — whichever pump ran first would consume them — and the failure would look like
// "input works when the window is idle and stops when it is being resized".
//
// `SDL_AddEventWatch` observes each event as it is added to the queue and consumes nothing, so this
// source and the display server coexist with **no change to the display server**, which is the
// arrangement the M4 ownership split asks for. The cost is stated rather than hidden: the callback
// runs on whichever thread pushed the event, so `attach()` must be called from the thread that
// pumps the platform and the `InputServer` it feeds must be the one that thread owns. That is the
// same single-producer contract `EventBuffer` documents.
//
// ================================================================================================
// WHAT "NORMALISED" MEANS HERE, AND WHAT IT DELIBERATELY DOES NOT DO
// ================================================================================================
//
// Translation only: an SDL scancode becomes a `cy::input::Key`, an SDL gamepad button becomes a
// vendor-neutral `GamepadControl`, an axis's `int16` becomes a float in [-1, 1] (or [0, 1] for a
// trigger), and SDL's nanosecond timestamp is carried through unchanged.
//
// **Nothing is coalesced and nothing is filtered.** A key that goes down and up between two pumps
// is two events here and two events in the accumulation window, which is what makes design.md §5's
// requirement achievable at all: information destroyed at this layer cannot be recovered above it.
// SDL's own key-repeat events are the one exception and they are dropped, because a repeat is not a
// transition — the key never came up — and forwarding it would report a press the player did not
// make. `TriggerKind::Pulse` is how a game gets repetition, on its own terms.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/servers/input/server.h>

namespace cy {

/// Feeds an `input::InputServer` from SDL3's event stream.
///
/// Owns nothing but its registration and the device identities it created. Constructing one does
/// nothing; `attach()` installs the watch and connects the keyboard and the mouse, `detach()`
/// removes it. Both are idempotent.
class Sdl3InputSource {
public:
    static constexpr u32 kMaxGamepads = 8;

    Sdl3InputSource() = default;
    ~Sdl3InputSource();

    Sdl3InputSource(const Sdl3InputSource&) = delete;
    Sdl3InputSource& operator=(const Sdl3InputSource&) = delete;

    /// Install the watch and register the keyboard and mouse as connected devices.
    ///
    /// Assigning them to a user is the caller's decision, not this class's: `input-and-actions`
    /// requires device assignment to be **explicit**, and a platform layer that assigned the
    /// keyboard to player one would be exactly the assumption the requirement forbids.
    ///
    /// Fails when SDL's event subsystem is not up. It does **not** fail for want of a device — a
    /// machine with no gamepad is the ordinary case and a machine with no keyboard is a dedicated
    /// server.
    [[nodiscard]] Status attach(input::InputServer& server) noexcept;
    void detach() noexcept;

    [[nodiscard]] bool attached() const noexcept { return server_ != nullptr; }

    [[nodiscard]] input::DeviceId keyboard() const noexcept { return keyboard_; }
    [[nodiscard]] input::DeviceId mouse() const noexcept { return mouse_; }

    /// The device this source created for SDL's joystick instance id, or a null id.
    [[nodiscard]] input::DeviceId gamepad(u32 instance_id) const noexcept;

    /// How many events this source has forwarded. Reported so that "no input" can be separated into
    /// "the platform produced none" and "the action layer resolved none" — two very different bugs
    /// with the same symptom.
    [[nodiscard]] u64 forwarded() const noexcept { return forwarded_; }
    /// How many SDL events were seen and deliberately not forwarded: key repeats, and every event
    /// class this source does not translate.
    [[nodiscard]] u64 ignored() const noexcept { return ignored_; }

    /// SDL's `SDL_EventFilter`, reached through a file-local trampoline that has SDL's own
    /// signature — this header may not name `SDL_Event`, so the trampoline lives in the .cpp and
    /// this is the member it calls. Public for that reason and for no other; nothing else should.
    static bool watch(void* userdata, void* sdl_event) noexcept;

private:
    struct GamepadSlot {
        /// SDL's joystick instance id.
        u32 instance_id = 0;
        /// The `SDL_Gamepad*`, held as void* so that no SDL type reaches this header — the same
        /// rule sdl3_display_server.h keeps for `SDL_Window*`.
        void* handle = nullptr;
        input::DeviceId device;
    };

    void on_event(const void* sdl_event) noexcept;
    void open_gamepad(u32 instance_id, Nanoseconds timestamp) noexcept;
    void close_gamepad(u32 instance_id, Nanoseconds timestamp) noexcept;
    void forward(input::DeviceId device, input::Control control, f32 value,
                 Nanoseconds timestamp) noexcept;

    input::InputServer* server_ = nullptr;
    input::DeviceId keyboard_;
    input::DeviceId mouse_;
    GamepadSlot gamepads_[kMaxGamepads];
    u32 gamepad_count_ = 0;
    u64 forwarded_ = 0;
    u64 ignored_ = 0;
};

}  // namespace cy
