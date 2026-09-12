#pragma once
// STORMS as spatial phenomena, and LIGHTNING as an event nobody plays. M10 task 3.2.
//
// `weather-and-wind` — "Storm phenomena": storms "SHALL be modelled as spatial phenomena with
// identity, position, velocity, radius, intensity, and type — moving across the world and
// contributing to pressure, wind, precipitation, cloud coverage, and lightning potential"; "Many
// storms SHALL be able to exist independently, and a storm SHALL be a first-class object that can
// be queried, replicated, recorded, and scripted"; "Lightning SHALL be published as an EVENT with
// position, intensity, and a seed, consumed by effects, audio, illumination, and gameplay. Weather
// SHALL NOT play a sound or spawn an effect directly"; "Thunder timing SHALL be derived by the
// audio system from distance, not scheduled by weather."
//
// ================================================================================================
// WHAT IS NOT HERE, AND WHY THAT IS THE REQUIREMENT
// ================================================================================================
//
// There is no sound handle, no effect identifier, no light, and no delay. `LightningEvent` carries
// a position, an intensity, a seed and the tick it happened on, and `drain()` is how a consumer
// gets it. The audio system computes thunder's delay from the distance to the position — this
// module does not know the speed of sound and has no business knowing it.
//
// The SEED is what makes the event reconstructable rather than recorded: a bolt's geometry, its
// flicker and its branch pattern are derived from it, so a replay that carries the event carries
// the bolt. `simulation-and-determinism`'s counter-based streams are the derivation; nothing here
// invents a second one.
//
// ================================================================================================
// REPLICATION IS THE STORM, NOT THE SKY
// ================================================================================================
//
// "Networking SHALL replicate LOW-FREQUENCY AUTHORITATIVE STATE: storm identity, position,
// velocity, intensity, regional weather state, and events such as lightning. Clients SHALL
// reconstruct visual detail locally." `encode()` writes exactly those fields and `decode()`
// rebuilds a registry that answers identically. A cloud volume cannot be written by it because
// there is no cloud volume in this module to write — the forbidden "world-scale volumetric cloud
// field stored, streamed, or replicated" is prevented by the type rather than by a rule.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/epoch.h>
#include <cy/core/determinism/random.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/world/coordinates.h>

namespace cy::weather {

/// A storm's identity. Stable for the storm's life, replicated, recorded and scriptable by it.
struct StormId {
    u64 value = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(StormId, StormId) noexcept = default;
};

enum class StormType : u8 {
    /// Rain and wind, no lightning.
    RainBand = 0,
    Thunderstorm = 1,
    Squall = 2,
    Blizzard = 3,
    SandStorm = 4,
    /// A rotating storm: the strongest winds and the deepest low.
    Cyclone = 5,
    Project = 6,
};

[[nodiscard]] const char* storm_type_name(StormType type) noexcept;

/// One storm. Every member is authoritative and every member is replicated; there is nothing else
/// in the struct, which is what keeps a storm's bandwidth a constant.
struct Storm {
    StormId id;
    StormType type = StormType::RainBand;
    /// Centre, absolute metres. f64 because a storm crossing a hundred-kilometre world at f32 would
    /// move in 8 mm steps at the far edge and in 60 mm steps further out.
    world::WorldVec3d position;
    /// Metres per second, horizontal. x in `.x`, z in `.y`.
    Vec2 velocity{6.0F, 0.0F};
    /// Metres. The radius at which the storm's contribution falls to zero.
    f32 radius_metres = 8000.0F;
    /// [0, 1]. Scales every contribution: pressure drop, wind, rain, cloud, lightning rate.
    f32 intensity = 0.5F;
    /// The storm's own randomness. Every draw a storm makes — lightning position, strike timing —
    /// is a substream of this, so a replicated storm strikes in the same places on every peer.
    u64 seed = 0;
    /// Seconds since the storm was spawned. Carried so a replay that joins late knows how old it
    /// is, and because decay is a function of age.
    f32 age_seconds = 0.0F;
    /// Seconds after which the storm dissipates. Zero for a storm a script owns the life of.
    f32 lifetime_seconds = 0.0F;

    [[nodiscard]] bool expired() const noexcept {
        return lifetime_seconds > 0.0F && age_seconds >= lifetime_seconds;
    }
};

/// What a storm does to the weather at a position. Every member is a contribution to be ADDED to
/// the cell's state, which is what lets the inspector report "this much of the rain is that storm".
struct StormContribution {
    /// Hectopascals, negative: a storm is a low.
    f32 pressure_delta = 0.0F;
    /// Metres per second, world axes.
    Vec2 wind{0.0F, 0.0F};
    f32 precipitation_mm_per_hour = 0.0F;
    f32 cloud_coverage = 0.0F;
    /// [0, 1]: how likely a strike is here, per second, before the storm's own rate is applied.
    f32 lightning_potential = 0.0F;
    /// Metres of visibility removed.
    f32 visibility_loss_metres = 0.0F;
    /// How much of the storm's radius the position is inside, [0, 1]. Zero outside it.
    f32 influence = 0.0F;
};

/// The contribution of one storm at one position. Free, so a diagnostic and the step evaluate the
/// same function and the inspector's attribution cannot drift from the state.
[[nodiscard]] StormContribution storm_contribution_at(const Storm& storm, f64 x, f64 z) noexcept;

/// A strike. Position, intensity, seed — the three the specification names — plus the tick and the
/// storm, because a consumer that draws it wants to know which storm it belongs to and a replay
/// wants to know when it was.
struct LightningEvent {
    world::WorldVec3d position;
    f32 intensity = 1.0F;
    u64 seed = 0;
    u64 tick = 0;
    StormId storm;
};

/// Many storms, independently. The registry is the queryable, replicable, recordable, scriptable
/// object the specification asks for; a storm is a value inside it.
class StormRegistry {
public:
    explicit StormRegistry(Allocator& allocator) noexcept;

    StormRegistry(const StormRegistry&) = delete;
    StormRegistry& operator=(const StormRegistry&) = delete;

    /// SCRIPTED: a storm a mission spawned. The identity is the caller's, so a sequence can name
    /// the same storm in two missions and a replay can bind an event to it.
    [[nodiscard]] Status spawn(const Storm& storm) noexcept;
    [[nodiscard]] Status despawn(StormId id) noexcept;
    [[nodiscard]] const Storm* find(StormId id) const noexcept;
    [[nodiscard]] Span<const Storm> storms() const noexcept { return storms_.span(); }
    [[nodiscard]] usize size() const noexcept { return storms_.size(); }

    /// Move every storm, age it, retire the expired, and emit the strikes the interval earned.
    ///
    /// Lightning is drawn from `determinism::RandomStream`: one substream per storm, indexed by the
    /// tick, so the sequence of strikes is a pure function of (session seed, storm seed, tick) and
    /// two peers agree without exchanging a bolt.
    [[nodiscard]] Status advance(determinism::SimulationPoint at, f32 seconds,
                                 u64 session_seed) noexcept;

    /// The summed contribution of every storm at a position.
    [[nodiscard]] StormContribution contribution_at(f64 x, f64 z) const noexcept;
    /// The same, with each storm's share reported separately. `out` is filled with at most its own
    /// length and the count is returned, so a diagnostic can ask for the top few without
    /// allocating.
    [[nodiscard]] usize contributions_at(f64 x, f64 z, Span<StormId> ids,
                                         Span<StormContribution> out) const noexcept;

    /// Strikes not yet taken. Drained by effects, audio, illumination and gameplay alike — every
    /// consumer sees every event, because `drain()` copies rather than consumes per reader.
    [[nodiscard]] Status drain_lightning(Array<LightningEvent>& out) noexcept;
    [[nodiscard]] usize pending_lightning() const noexcept { return lightning_.size(); }

    /// The wire form: identity, type, position, velocity, radius, intensity, seed and age, for
    /// every storm. Appended to `out`, which is not cleared — a caller packing several subsystems
    /// into one message keeps its own buffer.
    [[nodiscard]] Status encode(Array<u8>& out) const noexcept;
    /// Rebuild from the wire form, replacing whatever was here.
    [[nodiscard]] Status decode(Span<const u8> bytes) noexcept;
    /// Bytes one storm occupies on the wire. A constant, which is the point: a storm's cost does
    /// not depend on how big it is or how long it has been going.
    [[nodiscard]] static u32 encoded_storm_bytes() noexcept;

private:
    [[nodiscard]] Status emit_strikes(const Storm& storm, determinism::SimulationPoint at,
                                      f32 seconds, u64 session_seed) noexcept;

    Allocator* allocator_;
    Array<Storm> storms_;
    Array<LightningEvent> lightning_;
};

}  // namespace cy::weather
