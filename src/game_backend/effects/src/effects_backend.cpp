// SPDX-License-Identifier: MIT
#include <cy/game_backend/effects_backend.h>

#include <cy/abi/errors.h>

namespace cy::game_backend {

EffectsAdapter::EffectsAdapter(Allocator& allocator, vfx::SimulationWorld& world) noexcept
    : world_(&world), effects_(allocator) {}

Status EffectsAdapter::add_effect(const char* name, const vfx::CompiledSystem& system,
                                  f32 spawn_seconds) noexcept {
    if (name == nullptr || name[0] == '\0') {
        return fail(ErrorCode::InvalidArgument, "an effect needs a name");
    }
    const Name interned = Name::intern(name);
    for (const Entry& entry : effects_) {
        if (entry.name == interned) {
            return fail(ErrorCode::AlreadyExists,
                        "an effect is already registered under that name");
        }
    }
    if (!(spawn_seconds >= 0.0F)) {
        return fail(ErrorCode::InvalidArgument, "an effect's spawn seconds are negative");
    }
    return effects_.push_back(Entry{interned, &system, spawn_seconds, 0});
}

u64 EffectsAdapter::played(CyEffect handle) const noexcept {
    return handle == CY_EFFECT_NULL || handle > effects_.size() ? 0U : effects_[handle - 1U].plays;
}

CyResult EffectsAdapter::find(const char* name, CyEffect& out) const noexcept {
    const Name wanted = Name::intern(name);
    for (usize index = 0; index < effects_.size(); ++index) {
        if (effects_[index].name == wanted) {
            out = static_cast<CyEffect>(index) + 1U;
            return CY_RESULT_OK;
        }
    }
    return abi::report(CY_RESULT_NOT_FOUND, "no effect is registered under that name");
}

CyResult EffectsAdapter::play(CyEffect effect, const CyEffectPlay& play) noexcept {
    if (effect == CY_EFFECT_NULL || effect > effects_.size()) {
        return abi::report(CY_RESULT_NOT_FOUND, "an effect this host did not hand out");
    }
    Entry& entry = effects_[effect - 1U];
    vfx::EffectSpawn spawn;
    spawn.position = Vec3{play.position[0], play.position[1], play.position[2]};
    spawn.scale = play.scale;
    spawn.parameter_values = Span<const f32>(play.parameters, play.parameter_count);
    // FIRE AND FORGET: `vfx-system`'s spawn that needs no handle management. The instance releases
    // itself when its last particle dies, so a game that plays a burst a shot holds nothing.
    spawn.release_on_completion = true;
    spawn.spawn_seconds = entry.spawn_seconds;
    const Expected<vfx::EffectHandle, Error> played = world_->play(*entry.system, spawn);
    if (!played) {
        ++refused_;
        return abi::report(played.error().code == ErrorCode::OutOfRange ? CY_RESULT_OUT_OF_RANGE
                                                                        : CY_RESULT_INTERNAL,
                           played.error().message);
    }
    ++played_;
    ++entry.plays;
    return CY_RESULT_OK;
}

void bind_effects(cy::abi::Host& host, EffectsAdapter* adapter) noexcept {
    host.game.effects = adapter;
}

}  // namespace cy::game_backend
