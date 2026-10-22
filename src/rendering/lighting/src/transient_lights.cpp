// SPDX-License-Identifier: MIT
#include <cy/rendering/lighting/transient_lights.h>

#include <cy/core/math/math.h>

#include <cmath>

namespace cy::rendering {

f32 transient_envelope(TransientFade fade, f32 age, f32 lifetime) noexcept {
    if (!(lifetime > 0.0F)) {
        return 1.0F;
    }
    const f32 remaining = math::clamp(1.0F - (age / lifetime), 0.0F, 1.0F);
    switch (fade) {
        case TransientFade::None:
            return remaining > 0.0F ? 1.0F : 0.0F;
        case TransientFade::Linear:
            return remaining;
        case TransientFade::Quadratic:
        case TransientFade::Count:
            break;
    }
    return remaining * remaining;
}

TransientLights::TransientLights(Allocator& allocator) noexcept
    : slots_(allocator), free_(allocator) {}

Status TransientLights::reserve(u32 capacity) noexcept {
    if (status_.live != 0) {
        return fail(ErrorCode::InvalidArgument,
                    "transient lights: the pool is resized only while no light is alive");
    }
    slots_.clear();
    free_.clear();
    if (Status sized = slots_.resize(capacity); !sized) {
        return sized;
    }
    if (Status sized = free_.reserve(capacity); !sized) {
        return sized;
    }
    // Lowest slot first off the back, so the first light takes slot 0.
    for (u32 index = capacity; index > 0; --index) {
        (void)free_.push_back(index - 1U);
    }
    status_.capacity = capacity;
    return ok();
}

Expected<TransientLight, Error> TransientLights::spawn(const TransientLightDesc& desc) noexcept {
    if (desc.lifetime_seconds < 0.0F || !std::isfinite(desc.lifetime_seconds)) {
        return fail(ErrorCode::InvalidArgument,
                    "transient lights: a lifetime is a non-negative number of seconds");
    }
    if (desc.light.kind == render::LightKind::Directional ||
        desc.light.kind >= render::LightKind::Count) {
        return fail(ErrorCode::InvalidArgument,
                    "transient lights: a light for a moment is a point, a spot or an area light");
    }
    if (free_.empty()) {
        ++status_.refused;
        return fail(ErrorCode::OutOfRange,
                    "transient lights: every slot is taken; reserve a larger pool");
    }
    const u32 index = free_.back();
    free_.pop_back();
    Slot& slot = slots_[index];
    slot.desc = desc;
    slot.age = 0.0F;
    slot.live = true;
    ++status_.live;
    ++status_.spawned;
    return (static_cast<u64>(slot.generation) << 32U) | static_cast<u64>(index);
}

u32 TransientLights::index_of(TransientLight light) const noexcept {
    const auto index = static_cast<u32>(light & 0xFFFFFFFFULL);
    const auto generation = static_cast<u32>(light >> 32U);
    if (light == kNoTransientLight || index >= slots_.size()) {
        return kNoSlot;
    }
    const Slot& slot = slots_[index];
    return slot.live && slot.generation == generation ? index : kNoSlot;
}

void TransientLights::release(u32 index) noexcept {
    Slot& slot = slots_[index];
    slot.live = false;
    // A new generation, so every handle to the light that was here is stale from now.
    ++slot.generation;
    if (slot.generation == 0) {
        slot.generation = 1;
    }
    (void)free_.push_back(index);
    --status_.live;
}

Status TransientLights::update(TransientLight light,
                               const render::LightDescription& desc) noexcept {
    const u32 index = index_of(light);
    if (index == kNoSlot) {
        return fail(ErrorCode::NotFound, "transient lights: the light is gone");
    }
    if (desc.kind == render::LightKind::Directional || desc.kind >= render::LightKind::Count) {
        return fail(ErrorCode::InvalidArgument,
                    "transient lights: a light for a moment is a point, a spot or an area light");
    }
    slots_[index].desc.light = desc;
    return ok();
}

Status TransientLights::kill(TransientLight light) noexcept {
    const u32 index = index_of(light);
    if (index == kNoSlot) {
        return fail(ErrorCode::NotFound, "transient lights: the light is gone");
    }
    release(index);
    ++status_.killed;
    return ok();
}

bool TransientLights::alive(TransientLight light) const noexcept {
    return index_of(light) != kNoSlot;
}

void TransientLights::advance(f32 seconds) noexcept {
    if (!(seconds > 0.0F)) {
        return;
    }
    for (u32 index = 0; index < slots_.size(); ++index) {
        Slot& slot = slots_[index];
        if (!slot.live) {
            continue;
        }
        slot.age += seconds;
        if (slot.desc.lifetime_seconds > 0.0F && slot.age >= slot.desc.lifetime_seconds) {
            release(index);
            ++status_.expired;
        }
    }
}

Expected<u32, Error> TransientLights::append_to(
    Array<render::LightDescription>& out) const noexcept {
    u32 appended = 0;
    for (u32 index = 0; index < slots_.size(); ++index) {
        const Slot& slot = slots_[index];
        if (!slot.live) {
            continue;
        }
        render::LightDescription light = slot.desc.light;
        light.intensity *= transient_envelope(slot.desc.fade, slot.age, slot.desc.lifetime_seconds);
        light.stable_id = kTransientLightIdBit |
                          ((static_cast<u64>(slot.generation) << 32U) | static_cast<u64>(index));
        // A flash is dynamic by definition: nothing of it can have been baked.
        light.mobility = render::LightMobility::Movable;
        if (Status added = out.push_back(light); !added) {
            return make_unexpected(added.error());
        }
        ++appended;
    }
    return appended;
}

}  // namespace cy::rendering
