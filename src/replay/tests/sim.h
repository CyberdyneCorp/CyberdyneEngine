#pragma once
// THE TOY SESSION SECTION 3'S SUITES DRIVE. M9 section 3.
//
// ================================================================================================
// WHY THERE IS A REAL SIMULATION IN HERE AND NOT A STUB
// ================================================================================================
//
// Every claim section 3 makes is a claim about REPRODUCIBILITY, and a stub cannot fail to reproduce
// itself. "A recorded replay reproduces the final state hash exactly" is worth exactly as much as
// the thing being reproduced: if the "simulation" is a counter, then a replay of it is a counter
// and the test has proved that addition is deterministic.
//
// So this fixture runs the engine's own pieces, wired the way a game wires them:
//
//   * a real `ecs::World` with two registered components,
//   * a real `gameplay::GameSession` with four participants, a real `ControlRegistry` with a
//   control
//     source per participant bound to its entity on the movement channel,
//   * a real `gameplay::CommandStream` with **one producer per participant**, so the merge order
//   and
//     the per-producer sequence numbers are a four-player session's and not one stream's,
//   * a real `determinism::StateSchema` and two real `determinism::StateCodec`s compiled for
//     `CodecPurpose::Hash` at `HashDetail::Fields`, so the state hash is a tree that can be
//     descended to a field rather than a number,
//   * a real `determinism::StateProvider` beside the entity state, so a restore that forgot the
//     provider half is a failure rather than a silence.
//
// Everything the simulation does is integer-exact: movement adds small integers to `f32`s, which is
// exact, and the decay is integer arithmetic. That is deliberate — the subject under test is the
// RECORD-AND-REPLAY machinery, and a scenario whose own arithmetic could drift would make a failure
// ambiguous between the two. The floating-point half of determinism is section 0's spike and
// section 2's profile, and it is measured there.
//
// ================================================================================================
// THE HASH TREE'S SHAPE, AND WHY IT IS THIS SHAPE
// ================================================================================================
//
//   World "session"
//     Archetype <subject> "Position"      one per component, so two entity nodes for one entity
//       Entity <id>                       never collide under one parent
//         Component <subject> "Position"
//           Field <id> "x" / "y"
//
// `StateCodec::hash_rows()` opens the Entity, Component and Field levels; the World and Archetype
// levels are this fixture's, because only the caller knows how its world is laid out.
// `determinism::localise()` reads Entity, Component and Field out of the descent, which is exactly
// the triple "entity 5, component Position, field y" that M9's third artefact claim is about.

#include "fixture.h"

#include <cy/core/determinism/codec.h>
#include <cy/core/determinism/provider.h>
#include <cy/core/determinism/state_schema.h>
#include <cy/ecs/world.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>

#include <cstring>

namespace cy::replay_test {

/// What a player controls. Two `f32` fields so a divergence has a field to be narrowed to.
struct Position {
    cy::f32 x = 0.0F;
    cy::f32 y = 0.0F;
};

/// A second component, so the narrowing has to pick the right one rather than the only one.
struct Health {
    cy::i32 value = 100;
    cy::u32 shield = 0;
};

inline constexpr cy::determinism::SchemaSubject kPositionSubject{101};
inline constexpr cy::determinism::SchemaSubject kHealthSubject{102};

/// What a player intends. The same shape `src/gameplay/tests/fixture.h` uses, for the same reason:
/// a command payload is intent and fits inline.
struct MoveIntent {
    cy::i32 dx = 0;
    cy::i32 dy = 0;
};

/// The effect kind the session offers. One is enough: the ledger's key is `(kind, instance, tick)`
/// and the case this fixture exists to drive is one effect offered twice at one tick.
inline constexpr cy::u64 kExplosionKind = 0xE7'0510'0000'0001ULL;

/// A provider's state, restored beside the entity state.
///
/// `replay-and-rollback`: "random streams and provider state SHALL be restored with entity state."
/// A rollback that restored the world and not this would leave the session's own accumulator ahead
/// of the world it describes, and `tests/test_rollback.cpp` checks the number rather than trusting
/// that the call was made.
class StepAccumulator final : public cy::determinism::StateProvider {
public:
    [[nodiscard]] const char* name() const noexcept override { return "step-accumulator"; }
    [[nodiscard]] cy::determinism::Participates participation() const noexcept override {
        return cy::determinism::Participates::Rollback | cy::determinism::Participates::Checkpoint |
               cy::determinism::Participates::Hash;
    }
    [[nodiscard]] cy::Status capture(cy::Array<cy::u8>& out) const noexcept override {
        for (cy::u32 byte = 0; byte < 8; ++byte) {
            if (cy::Status pushed =
                    out.push_back(static_cast<cy::u8>((value >> (byte * 8U)) & 0xFFU));
                !pushed) {
                return pushed;
            }
        }
        return cy::ok();
    }
    [[nodiscard]] cy::Status restore(cy::Span<const cy::u8> bytes) noexcept override {
        if (bytes.size() != 8) {
            return cy::fail(cy::ErrorCode::InvalidArgument, "step accumulator: wrong size");
        }
        value = 0;
        for (cy::u32 byte = 0; byte < 8; ++byte) {
            value |= static_cast<cy::u64>(bytes[byte]) << (byte * 8U);
        }
        return cy::ok();
    }

    cy::u64 value = 0;
};

/// Told about an effect the simulation wants to realise. The rollback suite plugs the ledger in
/// here; the playback suite passes nothing and the effects are simply not offered.
using EffectSink = void (*)(void* user, cy::u64 kind, cy::u64 instance) noexcept;

/// Four players, four entities, one command each per tick.
class ToySession {
public:
    static constexpr cy::u32 kPlayers = 4;

    ToySession() noexcept
        : session_(allocator(), 0x5EED'5EEDULL),
          control_(allocator()),
          commands_(allocator(), control_),
          world_(allocator()),
          schema_(allocator()),
          providers_(allocator()),
          position_codec_(allocator()),
          health_codec_(allocator()) {}

    ToySession(const ToySession&) = delete;
    ToySession& operator=(const ToySession&) = delete;

    /// Wire everything. Returns false rather than asserting, so a case can say which stage failed.
    [[nodiscard]] bool build() noexcept;

    // --- What the suites drive -------------------------------------------------------------------

    /// Record this tick's scripted commands into each player's own producer. A pure function of the
    /// tick, which is what makes two runs of the session comparable.
    [[nodiscard]] bool script(cy::u64 tick) noexcept;

    /// Commit the producers and run the systems. **The one step function**: playback, rollback and
    /// the live loop all call exactly this, which is `replay-and-rollback`'s "Re-simulation SHALL
    /// be identical in path to normal simulation".
    [[nodiscard]] bool step(cy::u64 tick) noexcept;

    /// One tick, scripted: `script()` then `step()`. What a live session does.
    [[nodiscard]] bool live_tick(cy::u64 tick) noexcept { return script(tick) && step(tick); }

    void set_effect_sink(EffectSink sink, void* user) noexcept {
        effects_ = sink;
        effect_user_ = user;
    }

    // --- State
    // ------------------------------------------------------------------------------------

    /// Build the full hierarchical hash. The expensive half, taken on demand.
    [[nodiscard]] bool hash(cy::determinism::StateHashTree& tree) noexcept;
    /// The root hash alone. The cheap half, taken every tick.
    [[nodiscard]] cy::u64 root_hash() noexcept;

    [[nodiscard]] Position position_of(cy::u32 player) const noexcept;
    [[nodiscard]] Health health_of(cy::u32 player) const noexcept;
    [[nodiscard]] bool set_health(cy::u32 player, const Health& value) noexcept;

    // --- Accessors the suites wire things to
    // -------------------------------------------------------

    [[nodiscard]] cy::ecs::World& world() noexcept { return world_; }
    [[nodiscard]] cy::gameplay::CommandStream& commands() noexcept { return commands_; }
    [[nodiscard]] const cy::determinism::StateProviderRegistry& providers() const noexcept {
        return providers_;
    }
    [[nodiscard]] cy::gameplay::GameplayContext context(cy::u64 tick) noexcept;
    [[nodiscard]] cy::u32 producer_of(cy::u32 player) const noexcept { return producers_[player]; }
    [[nodiscard]] cy::u64 participant_bits(cy::u32 player) const noexcept {
        return participants_[player].bits();
    }
    [[nodiscard]] cy::ecs::Entity entity_of(cy::u32 player) const noexcept {
        return entities_[player];
    }
    [[nodiscard]] StepAccumulator& accumulator() noexcept { return accumulator_; }
    [[nodiscard]] cy::u32 effects_offered() const noexcept { return offered_; }

    /// Feed recorded commands straight into the producers, for the rollback loop's `feed` hook.
    [[nodiscard]] bool feed(cy::Span<const cy::replay::LogRecord> records) noexcept;

private:
    [[nodiscard]] bool declare_schema() noexcept;
    [[nodiscard]] bool gather(Position* positions, Health* healths, cy::u64* ids) const noexcept;

    cy::gameplay::GameSession session_;
    cy::gameplay::ControlRegistry control_;
    cy::gameplay::CommandStream commands_;
    cy::ecs::World world_;
    cy::determinism::StateSchema schema_;
    cy::determinism::StateProviderRegistry providers_;
    cy::determinism::StateCodec position_codec_;
    cy::determinism::StateCodec health_codec_;
    StepAccumulator accumulator_;

    cy::gameplay::ParticipantId participants_[kPlayers];
    cy::gameplay::ControlSourceId sources_[kPlayers];
    cy::ecs::Entity entities_[kPlayers] = {};
    cy::u32 producers_[kPlayers] = {};
    cy::ecs::ComponentTypeId position_ = 0;
    cy::ecs::ComponentTypeId health_ = 0;
    cy::gameplay::CommandTypeId move_ = cy::gameplay::kInvalidCommandType;
    EffectSink effects_ = nullptr;
    void* effect_user_ = nullptr;
    cy::u32 offered_ = 0;
};

// --- Implementation
// --------------------------------------------------------------------------------
//
// Inline in the header because two suites share it and a second translation unit for two hundred
// lines of fixture would be a third place to keep the wiring in step.

inline bool ToySession::declare_schema() noexcept {
    const cy::determinism::StateField position_fields[] = {
        {"x", 1, offsetof(Position, x), cy::reflect::FieldKind::F32,
         cy::determinism::SimulationClass::Authoritative, cy::determinism::StateEncoding::Direct},
        {"y", 2, offsetof(Position, y), cy::reflect::FieldKind::F32,
         cy::determinism::SimulationClass::Authoritative, cy::determinism::StateEncoding::Direct},
    };
    const cy::determinism::StateField health_fields[] = {
        {"value", 1, offsetof(Health, value), cy::reflect::FieldKind::I32,
         cy::determinism::SimulationClass::Authoritative, cy::determinism::StateEncoding::Direct},
        {"shield", 2, offsetof(Health, shield), cy::reflect::FieldKind::U32,
         cy::determinism::SimulationClass::Authoritative, cy::determinism::StateEncoding::Direct},
    };
    if (!schema_
             .declare(kPositionSubject, "Position",
                      cy::Span<const cy::determinism::StateField>{position_fields, 2})
             .has_value()) {
        return false;
    }
    if (!schema_
             .declare(kHealthSubject, "Health",
                      cy::Span<const cy::determinism::StateField>{health_fields, 2})
             .has_value()) {
        return false;
    }
    schema_.freeze();
    return position_codec_.compile(schema_, kPositionSubject, cy::determinism::CodecPurpose::Hash)
               .has_value() &&
           health_codec_.compile(schema_, kHealthSubject, cy::determinism::CodecPurpose::Hash)
               .has_value();
}

inline bool ToySession::build() noexcept {
    if (!world_.initialize().has_value()) {
        return false;
    }
    auto registered_position =
        world_.components().register_builtin("Position", sizeof(Position), alignof(Position));
    auto registered_health =
        world_.components().register_builtin("Health", sizeof(Health), alignof(Health));
    if (!registered_position || !registered_health) {
        return false;
    }
    position_ = *registered_position;
    health_ = *registered_health;

    cy::gameplay::CommandDeclaration declaration;
    declaration.name = cy::Name::intern("Move");
    declaration.stable_id = 11;
    declaration.channel = cy::gameplay::channels::movement();
    auto declared = commands_.declare(declaration);
    if (!declared) {
        return false;
    }
    move_ = *declared;

    for (cy::u32 player = 0; player < kPlayers; ++player) {
        auto entity = world_.create();
        if (!entity) {
            return false;
        }
        entities_[player] = *entity;
        if (!world_.add(entities_[player], position_).has_value() ||
            !world_.add(entities_[player], health_).has_value()) {
            return false;
        }
        if (!world_
                 .set(entities_[player], position_,
                      Position{static_cast<cy::f32>(player), static_cast<cy::f32>(player * 2U)})
                 .has_value()) {
            return false;
        }
        if (!world_.set(entities_[player], health_, Health{100, player}).has_value()) {
            return false;
        }

        auto participant = session_.add_participant(cy::gameplay::ParticipantKind::RemoteHuman,
                                                    cy::Name::intern("player"));
        if (!participant) {
            return false;
        }
        participants_[player] = *participant;
        auto source = control_.create_source(cy::gameplay::ControlSourceKind::Human,
                                             participants_[player], cy::Name::intern("input"));
        if (!source) {
            return false;
        }
        sources_[player] = *source;
        if (!control_
                 .bind_entity(sources_[player], cy::gameplay::channels::movement(),
                              entities_[player])
                 .has_value()) {
            return false;
        }
        // ONE PRODUCER PER PARTICIPANT, opened in participant order. That is what makes the merge
        // order and the per-producer sequence numbers a four-player session's — and what a replay
        // has to reproduce for its log to be byte-identical rather than merely equivalent.
        auto producer = commands_.open_producer(cy::Name::intern("player-input"));
        if (!producer) {
            return false;
        }
        producers_[player] = *producer;
    }

    if (!providers_.add(accumulator_).has_value()) {
        return false;
    }
    providers_.finalize();
    return declare_schema();
}

inline cy::gameplay::GameplayContext ToySession::context(cy::u64 tick) noexcept {
    cy::gameplay::GameplayContext ctx;
    ctx.session = &session_;
    ctx.services = &session_.services();
    ctx.commands = &commands_;
    ctx.at.tick = tick;
    return ctx;
}

inline bool ToySession::script(cy::u64 tick) noexcept {
    for (cy::u32 player = 0; player < kPlayers; ++player) {
        cy::gameplay::Command command;
        command.type = move_;
        command.participant = participants_[player];
        command.source = sources_[player];
        command.target = entities_[player];
        command.provenance = cy::gameplay::Provenance{cy::gameplay::ControlSourceKind::Human,
                                                      static_cast<cy::u32>(player + 1)};
        // A pure function of (tick, player): two runs of the session issue identical intent, so any
        // difference between them is the machinery's rather than the script's.
        const MoveIntent intent{1, static_cast<cy::i32>((tick + player) % 3U) - 1};
        (void)command.set_payload(intent);
        if (!commands_.producer(producers_[player]).record(command).has_value()) {
            return false;
        }
    }
    return true;
}

inline bool ToySession::feed(cy::Span<const cy::replay::LogRecord> records) noexcept {
    for (const cy::replay::LogRecord& record : records) {
        const cy::gameplay::Command& command = record.command;
        cy::u32 producer = 0;
        for (cy::u32 player = 0; player < kPlayers; ++player) {
            if (participants_[player].bits() == command.participant.bits()) {
                producer = producers_[player];
                break;
            }
        }
        cy::gameplay::Command replayed = command;
        replayed.provenance.kind = cy::gameplay::ControlSourceKind::Replay;
        if (!commands_.producer(producer).record(replayed).has_value()) {
            return false;
        }
    }
    return true;
}

inline bool ToySession::step(cy::u64 tick) noexcept {
    commands_.commit(context(tick), tick);
    for (cy::u32 index = 0; index < commands_.committed_count(); ++index) {
        const cy::gameplay::Command& command = commands_.committed(index);
        MoveIntent intent;
        if (!command.read_payload(intent)) {
            return false;
        }
        Position position;
        const void* stored = world_.get(command.target, position_);
        if (stored == nullptr) {
            return false;
        }
        std::memcpy(&position, stored, sizeof(Position));
        // Small integers into `f32`: exact, and deliberately so — see the header.
        position.x += static_cast<cy::f32>(intent.dx);
        position.y += static_cast<cy::f32>(intent.dy);
        if (!world_.set(command.target, position_, position).has_value()) {
            return false;
        }
    }

    // A system that is not driven by commands, so a replay that produced the commands and skipped
    // the systems would still differ.
    if (tick % 7 == 0) {
        for (cy::u32 player = 0; player < kPlayers; ++player) {
            Health health = health_of(player);
            health.value -= 1;
            health.shield = static_cast<cy::u32>(health.value & 0x0F);
            if (!world_.set(entities_[player], health_, health).has_value()) {
                return false;
            }
        }
    }

    accumulator_.value = cy::determinism::fold_hash(accumulator_.value, tick);

    // The effects. `instance` is a function of SIMULATION STATE — the entity — and never a counter,
    // which is the ledger's own requirement and what `tests/test_ledger.cpp` demonstrates the
    // failure of.
    if (effects_ != nullptr) {
        for (cy::u32 player = 0; player < kPlayers; ++player) {
            const Position position = position_of(player);
            if (static_cast<cy::i64>(position.x) % 10 != 0) {
                continue;
            }
            ++offered_;
            effects_(effect_user_, kExplosionKind, entities_[player].bits());
        }
    }
    return true;
}

inline Position ToySession::position_of(cy::u32 player) const noexcept {
    Position value;
    const void* stored = world_.get(entities_[player], position_);
    if (stored != nullptr) {
        std::memcpy(&value, stored, sizeof(Position));
    }
    return value;
}

inline Health ToySession::health_of(cy::u32 player) const noexcept {
    Health value;
    const void* stored = world_.get(entities_[player], health_);
    if (stored != nullptr) {
        std::memcpy(&value, stored, sizeof(Health));
    }
    return value;
}

inline bool ToySession::set_health(cy::u32 player, const Health& value) noexcept {
    return world_.set(entities_[player], health_, value).has_value();
}

inline bool ToySession::gather(Position* positions, Health* healths, cy::u64* ids) const noexcept {
    for (cy::u32 player = 0; player < kPlayers; ++player) {
        positions[player] = position_of(player);
        healths[player] = health_of(player);
        ids[player] = entities_[player].bits();
    }
    return true;
}

inline bool ToySession::hash(cy::determinism::StateHashTree& tree) noexcept {
    Position positions[kPlayers];
    Health healths[kPlayers];
    cy::u64 ids[kPlayers];
    if (!gather(positions, healths, ids)) {
        return false;
    }
    if (!tree.begin(cy::determinism::HashLevel::World, 0, "session").has_value()) {
        return false;
    }
    if (!tree.begin(cy::determinism::HashLevel::Archetype, kPositionSubject.value, "Position")
             .has_value()) {
        return false;
    }
    if (!position_codec_
             .hash_rows(tree, positions, sizeof(Position), kPlayers, ids,
                        cy::determinism::HashDetail::Fields)
             .has_value()) {
        return false;
    }
    if (!tree.end().has_value()) {
        return false;
    }
    if (!tree.begin(cy::determinism::HashLevel::Archetype, kHealthSubject.value, "Health")
             .has_value()) {
        return false;
    }
    if (!health_codec_
             .hash_rows(tree, healths, sizeof(Health), kPlayers, ids,
                        cy::determinism::HashDetail::Fields)
             .has_value()) {
        return false;
    }
    if (!tree.end().has_value()) {
        return false;
    }
    // The provider half, so a restore that forgot it changes the hash rather than passing quietly.
    if (!tree.begin(cy::determinism::HashLevel::Subsystem, 1, "step-accumulator").has_value()) {
        return false;
    }
    tree.mix_u64(accumulator_.value);
    if (!tree.end().has_value()) {
        return false;
    }
    return tree.end().has_value();
}

inline cy::u64 ToySession::root_hash() noexcept {
    cy::determinism::StateHashTree tree(allocator());
    if (!hash(tree)) {
        return 0;
    }
    return tree.root_hash();
}

}  // namespace cy::replay_test
