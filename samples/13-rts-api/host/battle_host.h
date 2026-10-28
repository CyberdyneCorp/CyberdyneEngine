// SPDX-License-Identifier: MIT
#ifndef CY_SAMPLE_RTS_API_BATTLE_HOST_H
#define CY_SAMPLE_RTS_API_BATTLE_HOST_H
// battle_host.h — what the host does for the battle (`--battle`), which is everything but deciding.
// ABI 1.16, openspec/changes/add-rts-combat-and-minimap.
//
// THE GAME FIGHTS THE BATTLE (game/Battle.swift, through `Lockstep.*`): it spawns a soldier for
// every unit, enlists and arms it, places red's buildings, gives every order, and reads every blow,
// flight and death back to play effects, flash lights and sound cues where blue's fog of war lets
// blue see them. The host only provides:
//
//   content       `/Level/Battlefield`, the node the game looks for to know there is a battle, and
//                 `units/soldier`, the prefab it spawns a soldier from
//   presentation  after every tick, each soldier's node stands where its unit stands — so the fog
//                 of war, which places a vision by its entity's node, sees from the battle — and a
//                 fallen unit's node stays where it fell
//   the look      the five effects (battle_effects.h) and their simulation world behind ABI 1.15,
//                 a pool of transient lights behind ABI 1.11, and five generated cues
//   the report    the session's battle as it stands, and what the game did with it
//
// The field lies at the RTS test map's `company` placement whether or not the map is drawn, and in
// a battle the fog's grid covers the map's whole terrain, so a run headless and a run drawn on the
// map see the same.

#include <cy/abi/host.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/game_backend/audio_backend.h>
#include <cy/game_backend/lights_backend.h>
#include <cy/game_backend/lockstep_session.h>
#include <cy/scene/scene.h>
#include <cy/scene/tree.h>
#include <cy/servers/audio/server.h>
#include <cy/servers/render/model.h>

#include <memory>
#include <vector>

#include "battle.h"

#if defined(CY_RTS_API_EFFECTS)
#    include <cy/game_backend/effects_backend.h>

#    include "battle_effects.h"
#endif

namespace sample::rts {

/// The names game/Contract.swift's `Content` and `BattleLook` use.
inline constexpr const char* kBattlefieldNode = "Battlefield";
inline constexpr const char* kSoldierPrefab = "units/soldier";
inline constexpr const char* kBattleCues[] = {"battle.clash", "battle.bow", "battle.throw",
                                              "battle.boom", "battle.fall"};
inline constexpr cy::u32 kBattleCueCount = 5;
/// The transient lights the game's explosions may hold at once.
inline constexpr cy::u32 kBattleLightCapacity = 32;

/// The soldier prefab: one node named "Soldier". Static storage; valid for the process.
[[nodiscard]] cy::scene::SceneDescription soldier_prefab() noexcept;

/// Cue `which` of `kBattleCues`, generated: the same samples on every machine.
[[nodiscard]] cy::Status make_battle_sound(cy::u32 which, cy::u32 sample_rate,
                                           cy::Array<cy::f32>& out) noexcept;

/// Where a field position stands in the level's metres.
[[nodiscard]] cy::Vec3 field_to_level(cy::detmath::FixedVec2 position) noexcept;

/// The battle as the report states it.
struct BattleObservation {
    bool on = false;
    cy::u32 units = 0;
    cy::u32 structures = 0;
    cy::u64 ticks = 0;
    cy::u64 events = 0;
    cy::u64 commands = 0;
    cy::u64 digest = 0;
    cy::u64 follower_digest = 0;
    cy::u32 disagreements = 0;
    cy::u32 blue_alive = 0;
    cy::u32 red_alive = 0;
    cy::u32 standing = 0;
    /// Points every hit of the battle dealt, from the session's events.
    cy::u64 damage = 0;
    cy::u32 peak_projectiles = 0;
    /// The look: effects the adapter played, lights the pool made, the most particles alive.
    cy::u64 effects = 0;
    cy::u64 lights = 0;
    cy::u32 lights_peak = 0;
    cy::u32 particles_peak = 0;
    /// What the game reported (`RtsReport`'s `battle*` fields).
    struct Game {
        cy::f32 soldiers = 0.0F;
        cy::f32 orders = 0.0F;
        cy::f32 events = 0.0F;
        cy::f32 damage = 0.0F;
        cy::f32 deaths = 0.0F;
        cy::f32 fallen = 0.0F;
        cy::f32 shown = 0.0F;
        cy::f32 hidden = 0.0F;
        cy::f32 effects = 0.0F;
        cy::f32 lights = 0.0F;
        cy::f32 sounds = 0.0F;
        cy::f32 trails = 0.0F;
    };
    Game game;
};

class BattleStage {
public:
    BattleStage(cy::Allocator& allocator, cy::scene::SceneTree& tree) noexcept;
    ~BattleStage();

    BattleStage(const BattleStage&) = delete;
    BattleStage& operator=(const BattleStage&) = delete;

    /// The cues, the effects and the lights; `bind` puts the last two on the ABI host.
    [[nodiscard]] cy::Status start(cy::audio::AudioServer& audio,
                                   cy::game_backend::AudioAdapter& cues,
                                   cy::u32 sample_rate) noexcept;
    void bind(cy::abi::Host& host) noexcept;
    static void unbind(cy::abi::Host& host) noexcept;

    /// Before the game's frame: last frame's lights age.
    void begin_frame(cy::f32 seconds) noexcept;
    /// After every session tick: each soldier's node where its unit stands, and the tick's tally.
    [[nodiscard]] cy::Status after_tick(const cy::game_backend::LockstepSession& session) noexcept;
    /// After the game's frame: the effects it played advance.
    [[nodiscard]] cy::Status end_frame(cy::f32 seconds) noexcept;

#if defined(CY_RTS_API_EFFECTS)
    /// What a renderer draws: the live sprites, and the lights the game's flashes hold.
    [[nodiscard]] cy::Status publish_particles(
        cy::Vec3 eye, cy::Array<cy::rendering::particles::ParticleInstance>& out) const noexcept;
#endif
    [[nodiscard]] cy::Status append_lights(
        cy::Array<cy::render::LightDescription>& out) const noexcept;

    void observe(const cy::game_backend::LockstepSession& session,
                 BattleObservation& out) const noexcept;

private:
    cy::Allocator* allocator_;
    cy::scene::SceneTree* tree_;
    std::vector<cy::Array<cy::f32>> sounds_;
    cy::game_backend::LightsAdapter lights_;
#if defined(CY_RTS_API_EFFECTS)
    std::unique_ptr<BattleEffects> effects_;
    std::unique_ptr<cy::game_backend::EffectsAdapter> effects_adapter_;
#endif
    cy::u64 damage_ = 0;
    cy::u32 peak_projectiles_ = 0;
    cy::u32 lights_peak_ = 0;
};

}  // namespace sample::rts

#endif  // CY_SAMPLE_RTS_API_BATTLE_HOST_H
