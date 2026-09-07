#include <cy/servers/residency/importance.h>

#include <cmath>

namespace cy::residency {

namespace {

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    if (!(value > 0.0F)) {  // also catches NaN, which must not become an importance
        return 0.0F;
    }
    return (value > 1.0F) ? 1.0F : value;
}

const ImportanceTransform kIdentityTransform{};

[[nodiscard]] bool valid(Subsystem subsystem) noexcept {
    return static_cast<u32>(subsystem) < kSubsystemCount;
}

}  // namespace

f32 compute_render_importance(const ImportanceInputs& inputs) noexcept {
    // COVERAGE IS AN AREA AND IMPORTANCE FOLLOWS EXTENT. A quad covering a quarter of the screen is
    // half as wide as one covering all of it, not a quarter as important, and the square root is
    // what stops a mid-distance instance from scoring two orders of magnitude below a near one.
    const f32 extent = std::sqrt(clamp01(inputs.screen_coverage));

    // THE GAMEPLAY COMPONENT RAISES, IT DOES NOT AVERAGE. "Systems can mark what matters without
    // knowing how each consumer will use it": a mark of 1 means *this is a hero unit*, and
    // averaging it with a small screen coverage would answer "half a hero", which is the outcome
    // that makes gameplay code go and invent its own importance somewhere else.
    //
    // Distance is deliberately NOT folded in here. `residency` derives the shared value from
    // coverage and the gameplay mark; distance appears in its example of a *declared transform*
    // ("shadows weight distance more strongly than textures do"), and putting it in both places
    // would apply it twice for the subsystem that asked for it once.
    const f32 gameplay = clamp01(inputs.gameplay_importance);
    return clamp01(extent + (gameplay * (1.0F - extent)));
}

f32 apply_transform(const ImportanceTransform& transform, f32 shared,
                    f32 distance_normalised) noexcept {
    const f32 distance = clamp01(distance_normalised);
    const f32 attenuation = 1.0F - (clamp01(transform.distance_weight) * distance);
    const f32 weighted = clamp01(shared * attenuation);
    const f32 shifted = clamp01((weighted * transform.gain) + transform.bias);
    if (transform.exponent == 1.0F || shifted == 0.0F) {
        return shifted;
    }
    return clamp01(std::pow(shifted, transform.exponent));
}

Status ImportanceTable::declare_transform(Subsystem subsystem,
                                          const ImportanceTransform& transform) noexcept {
    if (!valid(subsystem)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "residency: transform for an unknown subsystem"});
    }
    if (!(transform.exponent > 0.0F)) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "residency: an importance transform's exponent must be "
                                     "positive; a zero or negative one inverts the ordering"});
    }
    transforms_[static_cast<u32>(subsystem)] = transform;
    return ok();
}

const ImportanceTransform& ImportanceTable::transform(Subsystem subsystem) const noexcept {
    if (!valid(subsystem)) {
        return kIdentityTransform;
    }
    return transforms_[static_cast<u32>(subsystem)];
}

Status ImportanceTable::publish(u64 instance, const ImportanceInputs& inputs) noexcept {
    ImportanceInputs stored;
    stored.screen_coverage = clamp01(inputs.screen_coverage);
    stored.gameplay_importance = clamp01(inputs.gameplay_importance);
    stored.distance_normalised = clamp01(inputs.distance_normalised);
    if (auto placed = instances_.insert(instance, stored); !placed) {
        return make_unexpected(placed.error());
    }
    return ok();
}

bool ImportanceTable::retire(u64 instance) noexcept {
    return instances_.remove(instance);
}

void ImportanceTable::clear() noexcept {
    instances_.clear();
}

f32 ImportanceTable::shared(u64 instance) const noexcept {
    const ImportanceInputs* found = instances_.find(instance);
    return (found == nullptr) ? 0.0F : compute_render_importance(*found);
}

f32 ImportanceTable::for_subsystem(u64 instance, Subsystem subsystem) const noexcept {
    const ImportanceInputs* found = instances_.find(instance);
    if (found == nullptr) {
        return 0.0F;
    }
    return apply_transform(transform(subsystem), compute_render_importance(*found),
                           found->distance_normalised);
}

const ImportanceInputs* ImportanceTable::inputs(u64 instance) const noexcept {
    return instances_.find(instance);
}

}  // namespace cy::residency
