// SPDX-License-Identifier: MIT
// fog.cpp — the level's fog of war. See fog.h.

#include "fog.h"

#include "units.h"

namespace sample::rts {

RtsFog::RtsFog(cy::Allocator& allocator) noexcept : adapter_(allocator) {}

cy::Status RtsFog::build(const cy::Aabb& bounds) noexcept {
    if (cy::Status made = adapter_.create(fog_grid_over(bounds)); !made) {
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
    x_raw = fog_raw(at.x);
    z_raw = fog_raw(at.z);
    return true;
}

}  // namespace sample::rts
