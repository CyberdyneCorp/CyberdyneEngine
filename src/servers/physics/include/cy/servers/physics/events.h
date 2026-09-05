#pragma once
// Collision and trigger events, and the buffer a step writes them into. Task 4.2.4.
//
// `physics` — "Collision events": six channels — `CollisionEnter`, `CollisionStay`,
// `CollisionExit`, `TriggerEnter`, `TriggerStay`, `TriggerExit` — "each carrying the entity pair,
// contact points, normals, and impulses where applicable", "written during the physics step and
// readable in the same tick's `Simulation` stage".
//
// SIX CHANNELS, ONE RECORD. A `ContactEvent` carries a phase and a trigger flag rather than being
// six structs, because the requirement's real content is its first scenario: "both a system reading
// the channel and a behaviour receiving the callback SHALL observe identical data in the same
// tick". Identical data is easiest to guarantee when there is one record and two readers of it. The
// ECS event channels and the behaviour callbacks (`onCollisionEnter`, `onTriggerEnter`) are both
// projections of this buffer, and neither one is the original.
//
// WHY THE PAIR IS ORDERED. `a` is always the lower body handle. Without that rule a pair produces
// one event in one run and the mirrored event in another, depending on which body the broad phase
// visited first — which is a divergence that only appears under load, and exactly the class of bug
// `simulation-and-determinism` exists to make impossible. Callers that care which body is "theirs"
// compare handles; `other_than()` below does it for them.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/servers/physics/handles.h>

namespace cy::physics {

enum class ContactPhase : u8 {
    /// The pair began touching this tick.
    Enter = 0,
    /// The pair was already touching and still is.
    Stay,
    /// The pair stopped touching this tick. Contact points are not meaningful.
    Exit,
};

const char* contact_phase_name(ContactPhase value) noexcept;

/// The most contact points one event carries.
///
/// Four is a box resting flat on another box, which is the worst case that still means something to
/// a gameplay reader. A manifold with more points is truncated and `point_count` says so; the
/// solver's own manifold is not what an event is for.
inline constexpr u32 kMaxContactPoints = 4;

struct ContactPoint {
    /// World space, on the surface of `b`.
    Vec3 position{0.0f, 0.0f, 0.0f};
    /// Unit, pointing from `a` towards `b`.
    Vec3 normal{0.0f, 1.0f, 0.0f};
    /// Metres of overlap. Positive when the two interpenetrate.
    f32 penetration = 0.0f;
    /// Newton-seconds along the normal, for this point. Zero for a trigger and for `Exit`.
    f32 normal_impulse = 0.0f;
};

struct ContactEvent {
    /// Ordered: `a.bits() < b.bits()`. See the header comment.
    BodyHandle a;
    BodyHandle b;
    /// The two bodies' `user_data`, carried so a reader does not have to ask the server for them
    /// while it is walking a buffer of thousands.
    UserData user_data_a = 0;
    UserData user_data_b = 0;
    ContactPhase phase = ContactPhase::Enter;
    /// True when either collider is a sensor: the `Trigger*` channels rather than the `Collision*`
    /// ones.
    bool trigger = false;
    u32 point_count = 0;
    ContactPoint points[kMaxContactPoints] = {};
    /// The sum of the points' normal impulses. What a contact-impulse threshold is compared
    /// against, and what an impact sound scales with.
    f32 total_impulse = 0.0f;

    /// The other body of the pair. Returns a null handle when `self` is neither.
    [[nodiscard]] constexpr BodyHandle other_than(BodyHandle self) const noexcept {
        if (self == a) {
            return b;
        }
        if (self == b) {
            return a;
        }
        return BodyHandle{};
    }
};

/// The events one step produced.
///
/// Cleared at the start of every step and filled during it, so a reader in the same tick's
/// `Simulation` stage sees exactly that tick's events and a reader that skipped a tick sees nothing
/// of it. Capacity is reserved at world creation: an event buffer that reallocates mid-step is an
/// allocation inside the solve, and `physics`' step budget has no room for one.
class EventBuffer {
public:
    explicit EventBuffer(Allocator& allocator) noexcept : events_(allocator) {}

    /// Fix the buffer's size. `Array::reserve` rounds up — its minimum growth step is eight — so
    /// the RESERVATION is recorded separately: a caller that asked for room for two events and got
    /// a buffer that quietly held eight would discover the difference as a dropped event count of
    /// zero on a world whose configuration is wrong.
    [[nodiscard]] Status reserve(u32 capacity) noexcept {
        capacity_ = capacity;
        return events_.reserve(capacity);
    }

    void clear() noexcept {
        events_.clear();
        dropped_ = 0;
    }

    /// Append, ordering the pair. Returns false and counts a drop when the reservation is full —
    /// dropping rather than growing, because the alternative is an allocation inside the step.
    bool push(ContactEvent event) noexcept;

    [[nodiscard]] Span<const ContactEvent> events() const noexcept { return events_.span(); }
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(events_.size()); }
    /// Events the reservation could not hold. Non-zero is a configuration problem and the
    /// diagnostics report names it.
    [[nodiscard]] u32 dropped() const noexcept { return dropped_; }

private:
    Array<ContactEvent> events_;
    u32 capacity_ = 0;
    u32 dropped_ = 0;
};

/// The pair identity a backend tracks between ticks to turn "touching" into enter/stay/exit.
///
/// Free function rather than a member so the reference backend and the Jolt backend derive the same
/// key from the same two handles and a test can hold them to it.
[[nodiscard]] constexpr u64 contact_pair_key(BodyHandle a, BodyHandle b) noexcept {
    const u64 lo = a.bits() < b.bits() ? a.bits() : b.bits();
    const u64 hi = a.bits() < b.bits() ? b.bits() : a.bits();
    // Slot indices are 32-bit and generations are 32-bit, so a pair does not fit in 64 bits
    // without mixing. This is a hash, and a collision costs one spurious Stay rather than a wrong
    // simulation, which is why it is acceptable where the ordering rule above is not.
    return (lo * 0x9E3779B97F4A7C15ULL) ^ (hi + 0xC2B2AE3D27D4EB4FULL);
}

}  // namespace cy::physics
