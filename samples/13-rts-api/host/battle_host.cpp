// SPDX-License-Identifier: MIT
// battle_host.cpp — the host's half of the battle. See battle_host.h.

#include "battle_host.h"

#include <cy/abi/host.h>
#include <cy/core/math/scalar.h>

#include <cmath>

namespace sample::rts {
namespace {

using cy::f32;
using cy::u32;
using cy::Vec3;

const cy::scene::NodeDesc kSoldierNodes[] = {
    cy::scene::NodeDesc{cy::Name::intern("Soldier"), cy::scene::NodeDesc::kNoParent, cy::Name{},
                        cy::Transform::identity(), true, true, cy::Name{}, cy::Name{}},
};

/// A sound: its length, the pitch of its tone, how much of it is noise, how fast it dies away.
struct Voice {
    f32 seconds;
    f32 tone_hz;
    f32 noise;
    f32 decay_per_second;
    f32 loudness;
};

/// `kBattleCues`, in order: a clash of blades, a bowstring, a catapult's arm, a blast, a fall.
constexpr Voice kVoices[kBattleCueCount] = {
    {0.18F, 1900.0F, 0.55F, 28.0F, 0.45F}, {0.22F, 330.0F, 0.25F, 18.0F, 0.35F},
    {0.35F, 70.0F, 0.6F, 9.0F, 0.6F},      {0.9F, 45.0F, 0.85F, 4.0F, 0.8F},
    {0.3F, 110.0F, 0.7F, 12.0F, 0.4F},
};

}  // namespace

cy::scene::SceneDescription soldier_prefab() noexcept {
    return cy::scene::SceneDescription{cy::Name::intern(kSoldierPrefab),
                                       cy::Span<const cy::scene::NodeDesc>(kSoldierNodes)};
}

cy::Status make_battle_sound(u32 which, u32 sample_rate, cy::Array<f32>& out) noexcept {
    if (which >= kBattleCueCount || sample_rate == 0) {
        return cy::fail(cy::ErrorCode::InvalidArgument, "battle: no such sound");
    }
    const Voice& voice = kVoices[which];
    const auto frames = static_cast<u32>(voice.seconds * static_cast<f32>(sample_rate));
    if (cy::Status sized = out.resize(frames); !sized) {
        return sized;
    }
    // A fixed linear congruential sequence for the noise and a phase stepped in whole samples:
    // the same bytes on every machine, as the arrival click's.
    u32 state = 0xBA77'1E00U + which;
    const f32 step = 6.2831853F * voice.tone_hz / static_cast<f32>(sample_rate);
    const f32 decay = std::exp(-voice.decay_per_second / static_cast<f32>(sample_rate));
    f32 envelope = voice.loudness;
    f32 phase = 0.0F;
    for (f32& sample : out) {
        state = (state * 1664525U) + 1013904223U;
        const f32 noise = (static_cast<f32>(state >> 8U) / 8388608.0F) - 1.0F;
        sample = envelope * ((voice.noise * noise) + ((1.0F - voice.noise) * std::sin(phase)));
        phase += step;
        phase = phase > 6.2831853F ? phase - 6.2831853F : phase;
        envelope *= decay;
    }
    return cy::ok();
}

Vec3 field_to_level(cy::detmath::FixedVec2 position) noexcept {
    const auto metres = [](cy::detmath::Fixed value) {
        return static_cast<f32>(static_cast<cy::f64>(value.raw) /
                                static_cast<cy::f64>(cy::detmath::Fixed::kOneRaw));
    };
    return Vec3{battle::kFieldX + metres(position.x), 0.0F, battle::kFieldZ + metres(position.y)};
}

BattleStage::BattleStage(cy::Allocator& allocator, cy::scene::SceneTree& tree) noexcept
    : allocator_(&allocator), tree_(&tree), lights_(allocator) {}

BattleStage::~BattleStage() = default;

cy::Status BattleStage::start(cy::audio::AudioServer& audio, cy::game_backend::AudioAdapter& cues,
                              u32 sample_rate) noexcept {
    sounds_.reserve(kBattleCueCount);
    for (u32 which = 0; which < kBattleCueCount; ++which) {
        sounds_.emplace_back(*allocator_);
        if (cy::Status made = make_battle_sound(which, sample_rate, sounds_.back()); !made) {
            return made;
        }
        cy::audio::ClipDescription clip;
        clip.name = cy::Name::intern(kBattleCues[which]);
        clip.samples = sounds_.back().data();
        clip.frame_count = static_cast<u32>(sounds_.back().size());
        clip.sample_rate = sample_rate;
        const auto created = audio.create_clip(clip);
        if (!created) {
            return cy::make_unexpected(created.error());
        }
        if (cy::Status added = cues.add_cue(cy::Name::intern(kBattleCues[which]), *created);
            !added) {
            return added;
        }
    }
    if (cy::Status made = lights_.create(kBattleLightCapacity); !made) {
        return made;
    }
#if defined(CY_RTS_API_EFFECTS)
    effects_ = std::make_unique<BattleEffects>(*allocator_);
    if (cy::Status built = effects_->build(); !built) {
        return built;
    }
    effects_adapter_ =
        std::make_unique<cy::game_backend::EffectsAdapter>(*allocator_, effects_->world());
    for (u32 index = 0; index < kBattleEffects; ++index) {
        if (cy::Status added =
                effects_adapter_->add_effect(BattleEffects::name(index), effects_->system(index),
                                             BattleEffects::spawn_seconds(index));
            !added) {
            return added;
        }
    }
#endif
    return cy::ok();
}

void BattleStage::bind(cy::abi::Host& host) noexcept {
    cy::game_backend::bind_lights(host, &lights_);
#if defined(CY_RTS_API_EFFECTS)
    cy::game_backend::bind_effects(host, effects_adapter_.get());
#endif
}

void BattleStage::unbind(cy::abi::Host& host) noexcept {
    cy::game_backend::bind_lights(host, nullptr);
#if defined(CY_RTS_API_EFFECTS)
    cy::game_backend::bind_effects(host, nullptr);
#endif
}

void BattleStage::begin_frame(f32 seconds) noexcept {
    lights_.advance(seconds);
}

cy::Status BattleStage::after_tick(const cy::game_backend::LockstepSession& session) noexcept {
    for (u32 unit = 0; unit < session.units(); ++unit) {
        const auto state = session.unit(unit);
        if (!state || state->entity == 0U) {
            continue;
        }
        // A soldier the game has already taken off the field has no node: nothing to move.
        const cy::scene::Node node = tree_->node(cy::abi::from_abi(state->entity));
        if (!node.valid()) {
            continue;
        }
        cy::Transform placed = node.local_transform();
        placed.translation = field_to_level(state->position);
        if (!node.set_local_transform(placed)) {
            return cy::fail(cy::ErrorCode::Internal, "battle: a soldier's node refused its place");
        }
    }
    for (const cy::game_backend::LockstepEvent& event : session.events()) {
        damage_ += event.kind == cy::game_backend::LockstepEventKind::Damage ? event.amount : 0U;
    }
    peak_projectiles_ =
        cy::math::max(peak_projectiles_, static_cast<u32>(session.projectiles().size()));
    return cy::ok();
}

cy::Status BattleStage::end_frame(f32 seconds) noexcept {
    lights_peak_ = cy::math::max(lights_peak_, lights_.pool().status().live);
#if defined(CY_RTS_API_EFFECTS)
    return effects_->step(seconds);
#else
    (void)seconds;
    return cy::ok();
#endif
}

#if defined(CY_RTS_API_EFFECTS)
cy::Status BattleStage::publish_particles(
    Vec3 eye, cy::Array<cy::rendering::particles::ParticleInstance>& out) const noexcept {
    return effects_->publish(eye, out);
}
#endif

cy::Status BattleStage::append_lights(cy::Array<cy::render::LightDescription>& out) const noexcept {
    const auto appended = lights_.append_to(out);
    return appended ? cy::ok() : cy::make_unexpected(appended.error());
}

void BattleStage::observe(const cy::game_backend::LockstepSession& session,
                          BattleObservation& out) const noexcept {
    out.on = true;
    out.units = session.units();
    out.structures = session.structures();
    out.ticks = session.tick();
    out.events = session.event_count();
    out.commands = session.log().size();
    out.digest = session.digest();
    out.damage = damage_;
    out.peak_projectiles = peak_projectiles_;
    for (u32 unit = 0; unit < session.units(); ++unit) {
        const auto state = session.combatant(unit);
        if (state && state->armed && state->alive) {
            out.blue_alive += state->team == battle::kBlue ? 1U : 0U;
            out.red_alive += state->team == battle::kRed ? 1U : 0U;
        }
    }
    for (u32 index = 0; index < session.structures(); ++index) {
        const auto state = session.structure(index);
        out.standing += state && state->standing ? 1U : 0U;
    }
    out.lights = lights_.pool().status().spawned;
    out.lights_peak = lights_peak_;
#if defined(CY_RTS_API_EFFECTS)
    out.effects = effects_adapter_ == nullptr ? 0U : effects_adapter_->played();
    out.particles_peak = effects_ == nullptr ? 0U : effects_->peak_particles();
#endif
}

}  // namespace sample::rts
