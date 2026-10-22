// SPDX-License-Identifier: MIT
// fog.cpp — the level's fog of war. See fog.h.

#include "fog.h"

#include <cmath>

#include "level.h"
#include "units.h"

namespace sample::rts {
namespace {

/// Metres as Q32.32, rounded to nearest: the one conversion a float position makes.
[[nodiscard]] cy::i64 to_raw(cy::f32 metres) noexcept {
    return static_cast<cy::i64>(std::llround(static_cast<cy::f64>(metres) * 4294967296.0));
}

}  // namespace

RtsFog::RtsFog(cy::Allocator& allocator) noexcept : adapter_(allocator) {}

cy::Status RtsFog::build() noexcept {
    cy::gameplay::fog::FogGridDescription grid;
    grid.width = kFogCells;
    grid.height = kFogCells;
    grid.cell_size_raw = to_raw(kLevelSize / static_cast<cy::f32>(kFogCells));
    if (cy::Status made = adapter_.create(grid); !made) {
        return made;
    }
    adapter_.set_placer(&RtsFog::locate, this);
    return cy::ok();
}

void RtsFog::place(CyEntity entity, cy::Vec3 at) {
    fixed_.emplace_back(entity, at);
}

void RtsFog::bind(cy::abi::Host& host) noexcept {
    cy::game_backend::bind_fog(host, &adapter_);
}

void RtsFog::unbind(cy::abi::Host& host) noexcept {
    cy::game_backend::bind_fog(host, nullptr);
}

cy::Status RtsFog::update(cy::u64 tick) noexcept {
    return adapter_.update(tick);
}

bool RtsFog::locate(CyEntity entity, cy::i64& x_raw, cy::i64& z_raw, void* user) noexcept {
    const auto* self = static_cast<const RtsFog*>(user);
    cy::Vec3 at{};
    bool found = false;
    for (const auto& [placed, where] : self->fixed_) {
        if (placed == entity) {
            at = where;
            found = true;
            break;
        }
    }
    if (!found && self->motion_ != nullptr) {
        found = self->motion_->position(entity, at);
    }
    if (!found) {
        return false;
    }
    x_raw = to_raw(at.x);
    z_raw = to_raw(at.z);
    return true;
}

}  // namespace sample::rts
