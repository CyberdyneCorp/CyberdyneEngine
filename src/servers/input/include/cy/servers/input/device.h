#pragma once
// Devices: their identity, their capabilities, their lifecycle, and who owns them. Task 4.1.1.
//
// `input-and-actions` — "Device lifecycle": connection, disconnection, reconnection, capability
// change and low battery are surfaced as events; reassignment on disconnection follows a **declared
// policy** rather than being decided implicitly; and reconnection restores a device to the user
// that held it "so a controller running out of battery does not shuffle players".
//
// --- WHY A DEVICE HAS TWO IDENTITIES -------------------------------------------------------------
//
// A `DeviceId` is generational and dies with the connection: it is what an event carries, and it
// must go stale so that an event from an unplugged controller cannot be attributed to the one
// plugged in after it.
//
// A `hardware_id` is a `Name` interned from whatever the platform can offer as stable across
// unplugging — a controller GUID, a device path. It is what makes *reconnection* possible at all:
// without it, a controller that comes back is a new device and the player it belonged to has been
// forgotten. The registry remembers the last owner per `hardware_id`, and the default policy hands
// the device back.
//
// The pair is the reason "an input user SHALL NOT be identified by a device index" is achievable.
// Identity lives on the user; the device is an attachment.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/servers/input/event.h>
#include <cy/servers/input/types.h>

namespace cy::input {

/// What a device can do, beyond having controls. Reported by the backend, re-reported when it
/// changes — a controller whose rumble motors fail, a gamepad that gains a gyro when a firmware
/// update lands.
enum class DeviceCapability : u16 {
    None = 0,
    Rumble = 1U << 0U,
    Gyroscope = 1U << 1U,
    Accelerometer = 1U << 2U,
    Touchpad = 1U << 3U,
    Battery = 1U << 4U,
    Pose = 1U << 5U,
    TextInput = 1U << 6U,
};

[[nodiscard]] constexpr u16 operator|(DeviceCapability a, DeviceCapability b) noexcept {
    return static_cast<u16>(static_cast<u16>(a) | static_cast<u16>(b));
}
[[nodiscard]] constexpr bool has_capability(u16 mask, DeviceCapability capability) noexcept {
    return (mask & static_cast<u16>(capability)) != 0;
}

/// What a backend says about a device when it connects.
struct DeviceDescription {
    DeviceKind kind = DeviceKind::Unknown;
    /// Stable across unplugging where the platform can offer one. Empty when it cannot, in which
    /// case reconnection cannot restore the pairing and the registry says so rather than guessing.
    Name hardware_id;
    /// For the interface and for a diagnostic: "Xbox Wireless Controller".
    Name display_name;
    u16 capabilities = 0;
    /// Charge in [0, 1], or negative when the device does not report one.
    f32 battery = -1.0F;
};

/// A device as the registry holds it.
struct DeviceRecord {
    DeviceId id;
    DeviceDescription description;
    bool connected = false;
    /// The user this device is assigned to, or `kNoUser`. Assignment is always explicit — see
    /// `input-and-actions`: "Device assignment SHALL be explicit".
    u32 user = 0xFFFFFFFFU;
    /// The raw value of every control, updated as events are applied. A backend cannot report a
    /// control this array has no room for: `kMaxControlCode` sizes it and the enumerations are
    /// static_asserted against it in types.h.
    f32 controls[kMaxControlCode] = {};
};

inline constexpr u32 kNoUser = 0xFFFFFFFFU;

/// What happens to a user whose device disappears. Declared, never inferred.
enum class DisconnectPolicy : u8 {
    /// Keep the user, keep the pairing, await the device. The default, and the one that makes a
    /// flat battery a pause rather than a player swap.
    HoldAndAwait = 0,
    /// Assign the first unassigned compatible device, if there is one.
    ReassignToAnother,
    /// Keep the user and raise the event; the game is expected to pause.
    Pause,
};

const char* disconnect_policy_name(DisconnectPolicy policy) noexcept;

/// Who owns the keyboard and the mouse when there are several users.
///
/// `input-and-actions`: ownership follows "a declared policy — exclusive to one user, shared, or
/// split — rather than an assumption that they belong to the first player". The assumption is the
/// bug: it is invisible with one player and wrong with two.
enum class KeyboardMousePolicy : u8 {
    /// One user owns both. Which user is stated, not assumed.
    Exclusive = 0,
    /// Every user receives keyboard and mouse events. What a couch game's menus want.
    Shared,
    /// The keyboard goes to one user and the mouse to another. Rare, and supported because the
    /// alternative is a game re-implementing it.
    Split,
};

const char* keyboard_mouse_policy_name(KeyboardMousePolicy policy) noexcept;

/// What happened to a device.
enum class DeviceLifecycle : u8 {
    Connected = 0,
    Disconnected,
    /// A device with a remembered `hardware_id` came back and the policy restored its pairing.
    Reconnected,
    CapabilityChanged,
    BatteryLow,
    Assigned,
    Unassigned,
    Count,
};

const char* device_lifecycle_name(DeviceLifecycle event) noexcept;

struct DeviceLifecycleEvent {
    DeviceLifecycle kind = DeviceLifecycle::Connected;
    DeviceId device;
    DeviceKind device_kind = DeviceKind::Unknown;
    /// The user affected, or `kNoUser`.
    u32 user = kNoUser;
    Nanoseconds timestamp = 0;
    /// `BatteryLow` only.
    f32 battery = -1.0F;
};

/// The devices the process knows about, and the pairings it remembers.
///
/// Not thread-safe and not meant to be: connection and disconnection are handled where the platform
/// is pumped, and assignment at the same point. `input-and-actions` requires evaluation to "take no
/// global lock", and the way to have no lock is to have no sharing.
class DeviceRegistry {
public:
    static constexpr u32 kMaxDevices = 64;
    /// How many pairings are remembered for reconnection. A machine with more controllers than this
    /// having been plugged in during one session forgets the oldest, which is a better failure than
    /// unbounded growth in a registry that lives for the process.
    static constexpr u32 kMaxRemembered = 64;

    explicit DeviceRegistry(Allocator& allocator) noexcept;

    DeviceRegistry(const DeviceRegistry&) = delete;
    DeviceRegistry& operator=(const DeviceRegistry&) = delete;

    /// A device appeared. Emits `Connected`, and `Reconnected` when a remembered pairing was
    /// restored under the current policy.
    [[nodiscard]] Expected<DeviceId, Error> connect(const DeviceDescription& description,
                                                    Nanoseconds timestamp) noexcept;

    /// A device went away. The user survives — see the header comment — and the policy decides
    /// whether anything is reassigned.
    void disconnect(DeviceId device, Nanoseconds timestamp) noexcept;

    /// The backend re-reported what a device can do.
    [[nodiscard]] Status set_capabilities(DeviceId device, u16 capabilities,
                                          Nanoseconds timestamp) noexcept;

    /// Charge in [0, 1]. Emits `BatteryLow` on the crossing, once, rather than every report.
    [[nodiscard]] Status set_battery(DeviceId device, f32 battery, Nanoseconds timestamp) noexcept;
    void set_low_battery_threshold(f32 fraction) noexcept { low_battery_ = fraction; }

    /// Assign explicitly. Refuses a device that is already another user's — sharing a *physical*
    /// device between users is what `KeyboardMousePolicy::Shared` is for, and it is answered at
    /// routing rather than by two owners of one record.
    [[nodiscard]] Status assign(DeviceId device, u32 user, Nanoseconds timestamp) noexcept;
    void unassign(DeviceId device, Nanoseconds timestamp) noexcept;

    /// Every device a user holds, appended to `out`. Allocation-free when `out` has capacity.
    [[nodiscard]] u32 devices_of(u32 user, DeviceId* out, u32 capacity) const noexcept;

    [[nodiscard]] DeviceRecord* find(DeviceId device) noexcept;
    [[nodiscard]] const DeviceRecord* find(DeviceId device) const noexcept;

    /// The raw value of a control, for the evaluation path. Zero for an unknown device — a binding
    /// referring to a device nobody has is not an error, it is a binding whose scheme is inactive.
    [[nodiscard]] f32 control_value(DeviceId device, Control control) const noexcept;

    /// Apply one event's value to the device's control state. Called by the resolver as it walks
    /// the accumulated events, which is why it is here rather than inside `push`.
    void apply(const DeviceEvent& event) noexcept;

    /// Zero every delta control on every device.
    ///
    /// A delta is a displacement that happened *within one window* and is consumed by the tick that
    /// read it. Leaving it latched would make one mouse flick move the character on every tick
    /// until the mouse moved again — the mirror image of the defect `Interpretation` exists for,
    /// and just as invisible in a manual test where the mouse never stops.
    void clear_deltas() noexcept;

    [[nodiscard]] u32 count() const noexcept { return count_; }
    [[nodiscard]] const DeviceRecord& at(u32 index) const noexcept { return records_[index]; }

    void set_disconnect_policy(DisconnectPolicy policy) noexcept { disconnect_policy_ = policy; }
    [[nodiscard]] DisconnectPolicy disconnect_policy() const noexcept { return disconnect_policy_; }

    void set_keyboard_mouse_policy(KeyboardMousePolicy policy, u32 keyboard_user,
                                   u32 mouse_user) noexcept;
    [[nodiscard]] KeyboardMousePolicy keyboard_mouse_policy() const noexcept {
        return keyboard_mouse_policy_;
    }

    /// Which user an event on this device should reach, or `kNoUser`.
    ///
    /// For a gamepad this is the assignment. For a keyboard or a mouse it is the declared policy,
    /// and `Shared` returns `kNoUser` to mean *every* user — the caller loops, because "every" is
    /// not a user index and pretending it is one is how the first-player assumption gets back in.
    [[nodiscard]] u32 route(DeviceId device, bool& shared) const noexcept;

    /// Lifecycle events since the last drain, in the order they happened.
    [[nodiscard]] const Array<DeviceLifecycleEvent>& lifecycle_events() const noexcept {
        return lifecycle_;
    }
    void clear_lifecycle_events() noexcept { lifecycle_.clear(); }

private:
    struct Remembered {
        Name hardware_id;
        u32 user = kNoUser;
    };

    [[nodiscard]] i32 index_of(DeviceId device) const noexcept;
    void emit(DeviceLifecycle kind, const DeviceRecord& record, u32 user, Nanoseconds timestamp,
              f32 battery) noexcept;
    void remember(Name hardware_id, u32 user) noexcept;
    [[nodiscard]] u32 recall(Name hardware_id) const noexcept;
    /// The `ReassignToAnother` half of the disconnect policy, extracted so `disconnect()` reads as
    /// the three policies it is rather than as one of them inline.
    void reassign_after_disconnect(u32 user, DeviceKind kind, Nanoseconds timestamp) noexcept;

    DeviceRecord records_[kMaxDevices];
    u32 count_ = 0;
    /// The generation each slot is on. Bumped on disconnect, so a stale `DeviceId` resolves to
    /// nothing rather than to the device that took the slot.
    u32 generations_[kMaxDevices] = {};

    Remembered remembered_[kMaxRemembered];
    u32 remembered_count_ = 0;

    Array<DeviceLifecycleEvent> lifecycle_;
    DisconnectPolicy disconnect_policy_ = DisconnectPolicy::HoldAndAwait;
    KeyboardMousePolicy keyboard_mouse_policy_ = KeyboardMousePolicy::Exclusive;
    u32 keyboard_user_ = 0;
    u32 mouse_user_ = 0;
    f32 low_battery_ = 0.15F;
};

}  // namespace cy::input
