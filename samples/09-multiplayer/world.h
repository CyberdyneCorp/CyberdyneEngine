#pragma once
// The artefact's own world. M9 section 6.
//
// ================================================================================================
// WHY THIS IS NOT `src/replay/tests/sim.h`
// ================================================================================================
//
// `sim.h` is a TEST FIXTURE: it lives under src/replay/tests/, it is compiled into two test
// binaries, and a sample that included it would be a sample that cannot be built without the test
// tree. The wiring order below is deliberately the same as `ToySession::build()`'s — section 3's
// report says that is what the sample must follow — and everything else here is the artefact's.
//
// ================================================================================================
// WHAT IS DIFFERENT, AND WHY EACH DIFFERENCE IS THE POINT
// ================================================================================================
//
//   * THE COMPONENTS ARE REFLECTED, not `register_builtin`. `ToySession` registers two components
//     by name and by size, which is all a hashing codec needs. This artefact ARMS THE DETERMINISM
//     FIREWALL (task 1.4b), and `declare_from_reflection()` can only derive authority from what a
//     component's fields declare — so a world of built-ins would report `guarded 0 of 4`, honestly
//     and uselessly. `Position` carries a `Replicated` attribute because it is the component this
//     session puts on the wire; `Health` carries `RuntimeState` persistence. Both are true of the
//     session rather than decorations chosen to make the number look good.
//
//   * THE INTENT COMES FROM OUTSIDE. `ToySession::script()` is a pure function of (tick, player)
//     and that is what makes a fixture comparable. Here a player's intent arrives over a lossy
//     network, or does not, and the session has to decide what to simulate — which is the whole
//     subject. `intent_of()` is still a pure function of (tick, player): it is what the PLAYER
//     pressed. What the HOST simulated is a different question and the artefact's first claim.
//
//   * THE ARITHMETIC IS INTEGER-EXACT, and for `sim.h`'s reason: the subject is the record, the
//     rollback and the localiser, and a scenario whose own arithmetic could drift would make every
//     failure ambiguous between the two. Section 0's spike is where floating point is measured.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/codec.h>
#include <cy/core/determinism/provider.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/core/memory/allocator.h>
#include <cy/ecs/world.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>
#include <cy/gameplay/firewall_arming.h>
#include <cy/replay/record.h>

namespace cy::mp {

/// Four players. The number is in the milestone's own sentence, so it is a constant rather than an
/// option: an artefact that demonstrated two would be demonstrating something else.
inline constexpr u32 kPlayers = 4;

/// What a player controls, and what goes on the wire.
struct Position {
    f32 x = 0.0F;
    f32 y = 0.0F;
};

/// A second component, so the divergence localiser has to pick the right one rather than the only
/// one.
struct Health {
    i32 value = 100;
    u32 shield = 0;
};

/// What a player intends for one tick. This is the payload the network carries.
struct MoveIntent {
    i32 dx = 0;
    i32 dy = 0;
};

inline constexpr determinism::SchemaSubject kPositionSubject{201};
inline constexpr determinism::SchemaSubject kHealthSubject{202};

/// The one effect kind. The ledger's key is `(kind, instance, tick)` and the case that matters is
/// one effect offered twice at one tick, which one kind is enough to produce.
inline constexpr u64 kExplosionKind = 0x0910'0000'0000'0001ULL;

/// What the player pressed at this tick. A pure function of (tick, player), so two runs of the
/// session are comparable and any difference between them belongs to the machinery.
[[nodiscard]] MoveIntent intent_of(u64 tick, u32 player) noexcept;

/// Told about an effect the simulation wants to realise. The session plugs the rollback engine in
/// here; a session with no rollback engine passes nothing and no effect is offered.
using EffectSink = void (*)(void* user, u64 kind, u64 instance) noexcept;

/// A provider's state, restored beside the entity state. `replay-and-rollback`: "random streams and
/// provider state SHALL be restored with entity state" — a rollback that restored the world and not
/// this would leave the session's accumulator ahead of the world it describes.
class StepAccumulator final : public determinism::StateProvider {
public:
    [[nodiscard]] const char* name() const noexcept override { return "step-accumulator"; }
    [[nodiscard]] determinism::Participates participation() const noexcept override;
    [[nodiscard]] Status capture(Array<u8>& out) const noexcept override;
    [[nodiscard]] Status restore(Span<const u8> bytes) noexcept override;

    u64 value = 0;
};

/// One machine's copy of the game: a world, four entities, a command stream with one producer per
/// player, and the two codecs that make the state hash a tree rather than a number.
class GameWorld {
public:
    GameWorld() noexcept;

    GameWorld(const GameWorld&) = delete;
    GameWorld& operator=(const GameWorld&) = delete;

    /// Register, arm, create. Returns false rather than asserting, so the caller can say which
    /// stage failed.
    [[nodiscard]] bool build() noexcept;

    /// **The startup line task 1.4b owes an artefact.** Valid after `build()`.
    [[nodiscard]] const gameplay::FirewallArmingReport& arming() const noexcept { return arming_; }

    /// Record one player's intent into that player's own producer. What a host does with an intent
    /// that arrived, and what a client does with the one it is predicting.
    [[nodiscard]] bool offer_intent(u32 player, u64 tick, const MoveIntent& intent) noexcept;

    /// Commit the producers and run the systems. **The one step function**: the live loop, the
    /// rollback loop and playback all call exactly this.
    [[nodiscard]] bool step(u64 tick) noexcept;

    /// One authoritative command, as a log record in THIS world's identifiers. What a client
    /// appends when the host tells it what really happened at a tick it has already predicted.
    [[nodiscard]] replay::LogRecord command_record(u32 player, u64 tick,
                                                   const MoveIntent& intent) const noexcept;

    /// Feed recorded commands straight into the producers, for the rollback loop's `feed` hook and
    /// for a session replaying somebody else's log.
    [[nodiscard]] bool feed(Span<const replay::LogRecord> records) noexcept;

    void set_effect_sink(EffectSink sink, void* user) noexcept;

    [[nodiscard]] bool hash(determinism::StateHashTree& tree) noexcept;
    [[nodiscard]] u64 root_hash() noexcept;

    [[nodiscard]] Position position_of(u32 player) const noexcept;
    [[nodiscard]] Health health_of(u32 player) const noexcept;
    [[nodiscard]] bool set_health(u32 player, const Health& value) noexcept;

    [[nodiscard]] ecs::World& world() noexcept { return world_; }
    [[nodiscard]] gameplay::CommandStream& commands() noexcept { return commands_; }
    [[nodiscard]] const determinism::StateProviderRegistry& providers() const noexcept {
        return providers_;
    }
    [[nodiscard]] u32 producer_of(u32 player) const noexcept { return producers_[player]; }
    [[nodiscard]] u64 participant_bits(u32 player) const noexcept {
        return participants_[player].bits();
    }
    [[nodiscard]] ecs::Entity entity_of(u32 player) const noexcept { return entities_[player]; }
    [[nodiscard]] u32 effects_offered() const noexcept { return offered_; }

private:
    [[nodiscard]] bool declare_schema() noexcept;
    [[nodiscard]] bool arm_firewall() noexcept;
    [[nodiscard]] gameplay::GameplayContext context(u64 tick) noexcept;

    gameplay::GameSession session_;
    gameplay::ControlRegistry control_;
    gameplay::CommandStream commands_;
    ecs::World world_;
    determinism::StateSchema schema_;
    determinism::StateProviderRegistry providers_;
    determinism::StateCodec position_codec_;
    determinism::StateCodec health_codec_;
    StepAccumulator accumulator_;
    gameplay::FirewallArmingReport arming_;

    gameplay::ParticipantId participants_[kPlayers];
    gameplay::ControlSourceId sources_[kPlayers];
    ecs::Entity entities_[kPlayers] = {};
    u32 producers_[kPlayers] = {};
    ecs::ComponentTypeId position_ = 0;
    ecs::ComponentTypeId health_ = 0;
    gameplay::CommandTypeId move_ = gameplay::kInvalidCommandType;
    EffectSink effects_ = nullptr;
    void* effect_user_ = nullptr;
    u32 offered_ = 0;
};

}  // namespace cy::mp
