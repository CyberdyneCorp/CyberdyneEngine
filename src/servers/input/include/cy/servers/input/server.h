#pragma once
// CyberInput's server: devices, users, contexts, the accumulation window, and the per-tick
// resolution that turns events into actions and command frames. Tasks 4.1.1 to 4.1.6.
//
// ================================================================================================
// WHAT THIS SERVER MAY NOT SEE, AND WHY THAT IS THE POINT
// ================================================================================================
//
// `src/servers/` is layer 2. There is no ECS world here, no scene node, no camera, no renderer and
// no gameplay type — the layer checker fails the build over any of them. Two requirements become
// properties of the build rather than rules somebody has to remember:
//
//   * `input-and-actions` — "an input layer that writes a transform" has crossed a line. It cannot:
//     there is no transform reachable from this header.
//   * "A reference frame modifier SHALL receive a semantic frame — forward, right, and up —
//     supplied by a provider such as the camera, and SHALL NOT depend on renderer internals."
//     `InputUser::set_reference_frame()` takes three vectors. There is no camera type to depend on.
//
// The converse boundary — gameplay not reading devices — is `gameplay-framework`'s and is enforced
// from the other side: `src/gameplay/` declares no dependency on this module, so a gameplay
// translation unit cannot include this header at all. See `src/gameplay/README.md`.
//
// ================================================================================================
// HEADLESS IS THE DEFAULT, NOT A MODE
// ================================================================================================
//
// "Input SHALL function with **no devices present**: a dedicated server, a test harness, or a
// replay has no keyboard, and the action and command path SHALL still operate from synthetic,
// network, replay, and artificial-intelligence sources. Absence of a device backend SHALL NOT
// prevent the system from initialising."
//
// So `initialize()` takes no backend and never fails for want of one. A device exists when
// something calls `devices().connect()`; nothing here polls for one. Every test in this module runs
// with no platform layer at all, which is what makes them tests of the model rather than of SDL.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/servers/input/action.h>
#include <cy/servers/input/binding.h>
#include <cy/servers/input/device.h>
#include <cy/servers/input/event.h>
#include <cy/servers/input/frame.h>
#include <cy/servers/input/user.h>

namespace cy::input {

struct InputServerConfig {
    /// How many local users. `input-and-actions` asks for at least 8.
    u32 users = 1;
    /// The accumulation window's capacity, in events. Overflow is counted and reported rather than
    /// silently dropped — see `EventBuffer`.
    u32 event_capacity = 1024;
    /// Whether synthetic and remote injection is permitted. A shipping build sets this false, which
    /// is `input-and-actions`' "a shipping build SHALL be able to disable them".
    bool allow_synthetic = true;
};

/// The input server.
///
/// The four methods `runtime::Server` declares — `backend_name`, `initialize`, `shutdown`,
/// `is_null_backend` — have the same names and signatures here, so the runtime's adapter is four
/// forwarding lines and no decisions. This class does not derive from that interface because
/// `runtime::Server` is layer 5 and this is layer 2; the render server has the same arrangement and
/// the same comment.
class InputServer {
public:
    explicit InputServer(Allocator& allocator) noexcept;
    ~InputServer();

    InputServer(const InputServer&) = delete;
    InputServer& operator=(const InputServer&) = delete;

    // --- The four methods `runtime::Server` declares ---------------------------------------------

    [[nodiscard]] const char* backend_name() const noexcept { return backend_name_; }
    [[nodiscard]] Status initialize() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] bool is_null_backend() const noexcept { return null_backend_; }

    void set_backend(const char* name, bool is_null) noexcept;

    /// Size the server. Refused after `initialize()`: the users and the window are allocated there
    /// and a capacity that changed underneath a half-resolved tick would strand the accumulation.
    [[nodiscard]] Status configure(const InputServerConfig& config) noexcept;
    [[nodiscard]] const InputServerConfig& configuration() const noexcept { return config_; }

    // --- Declarations ------------------------------------------------------------------------

    [[nodiscard]] ActionRegistry& actions() noexcept { return actions_; }
    [[nodiscard]] const ActionRegistry& actions() const noexcept { return actions_; }

    /// Register a mapping context. The server owns it and hands back a handle; the context is
    /// **immutable** from here on, which is what makes a player's override an override rather than
    /// an edit. See binding.h.
    [[nodiscard]] Expected<ContextHandle, Error> register_context(
        MappingContext&& context) noexcept;
    [[nodiscard]] const MappingContext* context(ContextHandle handle) const noexcept;

    /// Called once every action is declared. Sizes each user's records; refuses to run twice with a
    /// different action count, because a user's arrays are what the evaluation path indexes.
    [[nodiscard]] Status finalize_declarations() noexcept;

    // --- Users and devices -----------------------------------------------------------------------

    [[nodiscard]] u32 user_count() const noexcept { return static_cast<u32>(users_.size()); }
    [[nodiscard]] InputUser& user(u32 index) noexcept { return *users_[index]; }
    [[nodiscard]] const InputUser& user(u32 index) const noexcept { return *users_[index]; }

    [[nodiscard]] DeviceRegistry& devices() noexcept { return devices_; }
    [[nodiscard]] const DeviceRegistry& devices() const noexcept { return devices_; }

    /// Assign a device and tell the user about it. The pair has to happen together — a device
    /// assigned in the registry that the user does not know about is a device whose events route
    /// nowhere — so there is one entry point rather than two that a caller must remember to pair.
    [[nodiscard]] Status assign(DeviceId device, u32 user, Nanoseconds timestamp) noexcept;
    void unassign(DeviceId device, Nanoseconds timestamp) noexcept;

    /// Drain the registry's lifecycle events into the users' caches. Called by `resolve_tick`;
    /// exposed so a caller that wants to react to a disconnection before the tick can.
    void pump_device_lifecycle() noexcept;

    // --- The accumulation window
    // ------------------------------------------------------------------

    /// The door the platform layer pushes through. See `EventBuffer`: it never coalesces.
    void submit(const DeviceEvent& event) noexcept;

    /// Inject an action-shaped input without fabricating an operating-system event.
    ///
    /// `input-and-actions` — "Synthetic and remote input": a test "SHALL inject actions directly,
    /// with no operating-system event simulation", and the injection "SHALL be recorded as
    /// synthetic". Refused when `allow_synthetic` is false, which is how a shipping build turns it
    /// off; the refusal is an error rather than a silent drop, so a test harness in a shipping
    /// build fails loudly.
    [[nodiscard]] Status inject(DeviceId device, Control control, f32 value, Nanoseconds timestamp,
                                EventSource source = EventSource::Synthetic) noexcept;

    /// Create a virtual device to inject through: a test's, a replay's, a remote peer's.
    [[nodiscard]] Expected<DeviceId, Error> create_virtual_device(Name name,
                                                                  Nanoseconds timestamp) noexcept;

    [[nodiscard]] const EventBuffer& pending() const noexcept { return events_; }

    /// Push composed text. Distinct from the key stream by construction — see frame.h.
    void submit_text(const TextEvent& event) noexcept;
    [[nodiscard]] const Array<TextEvent>& text() const noexcept { return text_; }
    void clear_text() noexcept { text_.clear(); }

    // --- Resolution
    // -------------------------------------------------------------------------------

    /// Resolve one simulation tick from everything accumulated since the last call.
    ///
    /// THE ORDER MATTERS AND IT IS THE REQUIREMENT. Events are sorted by `(timestamp, sequence)`
    /// and applied one at a time, so a press and a release inside one window are two transitions on
    /// one action record rather than a level that happens to be back where it started. design.md
    /// §5 and `input-and-actions`' "A fast press is not lost".
    ///
    /// `now` is the tick's timestamp on the same clock the events carry. `delta_seconds` is the
    /// fixed step, used by time-based triggers and by `Smooth`; it never scales a value, which is
    /// `Interpretation`'s job.
    void resolve_tick(u64 tick, Nanoseconds now, f32 delta_seconds) noexcept;

    /// How many events the last resolution consumed, and how many the window dropped. Reported
    /// rather than internal: a dropped input is a defect a player feels.
    [[nodiscard]] u32 last_event_count() const noexcept { return last_events_; }
    [[nodiscard]] u32 dropped_events() const noexcept { return dropped_total_; }

    [[nodiscard]] u64 tick() const noexcept { return tick_; }

private:
    [[nodiscard]] Status rebuild_user(InputUser& target) noexcept;

    Allocator* allocator_;
    InputServerConfig config_;
    ActionRegistry actions_;
    DeviceRegistry devices_;
    EventBuffer events_;
    Array<TextEvent> text_;
    /// Contexts are individually allocated and never moved: a `ContextHandle` is resolved by index
    /// into this array and the `MappingContext` it names holds an `Array` a reallocation would
    /// move under a resolved table pointing at it.
    Array<MappingContext*> contexts_;
    Array<InputUser*> users_;
    const char* backend_name_ = "null";
    bool null_backend_ = true;
    bool initialised_ = false;
    u64 tick_ = 0;
    u32 last_events_ = 0;
    u32 dropped_total_ = 0;
    u32 virtual_devices_ = 0;
};

}  // namespace cy::input
