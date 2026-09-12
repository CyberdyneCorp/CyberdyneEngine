// Buoyancy from multiple sample points. M10 task 2.3.

#include <cy/water/buoyancy.h>

#include <cmath>

namespace cy::water {

namespace {

/// How submerged a sample is, in [0, 1]. A soft ramp over `softness` metres rather than a step:
/// a hull crossing the surface in one frame would otherwise gain its whole displacement force in
/// one step and ring at the integrator's frequency.
[[nodiscard]] f32 submersion_fraction(f32 submersion, f32 softness) noexcept {
    if (submersion <= 0.0F) {
        return 0.0F;
    }
    if (softness <= 0.0F) {
        return 1.0F;
    }
    const f32 fraction = submersion / softness;
    return (fraction >= 1.0F) ? 1.0F : fraction;
}

}  // namespace

Status hull_positions(const BuoyancyState& state, Span<const BuoyancySample> samples,
                      Span<world::WorldVec3d> out) noexcept {
    if (samples.size() != out.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "water: a hull's sample and position spans must be the same length");
    }
    for (usize index = 0; index < samples.size(); ++index) {
        const Vec3& offset = samples[index].offset;
        out[index] = world::WorldVec3d{state.position.x + static_cast<f64>(offset.x),
                                       state.position.y + static_cast<f64>(offset.y),
                                       state.position.z + static_cast<f64>(offset.z)};
    }
    return ok();
}

Expected<BuoyancyResult, Error> compute_buoyancy(const BuoyancyState& state,
                                                 Span<const BuoyancySample> samples,
                                                 Span<const WaterSample> water,
                                                 const BuoyancyParams& params) noexcept {
    if (samples.size() != water.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "water: buoyancy needs one water sample per hull sample, in the same order");
    }

    BuoyancyResult result;
    result.samples = static_cast<u32>(samples.size());
    Vec3 velocity_sum{0.0F, 0.0F, 0.0F};

    for (usize index = 0; index < samples.size(); ++index) {
        const BuoyancySample& sample = samples[index];
        const WaterSample& here = water[index];
        const f32 fraction = submersion_fraction(here.submersion, params.submersion_softness);
        if (fraction <= 0.0F || sample.volume <= 0.0F) {
            continue;
        }

        const f32 volume = sample.volume * fraction;
        result.submerged_volume += volume;
        ++result.submerged_samples;
        velocity_sum = velocity_sum + here.velocity;

        // Archimedes: the weight of the displaced fluid, upward. The density is the BODY's, which
        // is why a boat floats higher in the sea than in a lake without anything else changing.
        const Vec3 lift{0.0F, here.density * kGravity * volume, 0.0F};

        // The point's own velocity, including the rotation about the origin: a pitching hull's bow
        // is moving even when its centre is not, and drag that ignored that would not damp the
        // pitch at all.
        const Vec3 point_velocity = state.velocity + cross(state.angular_velocity, sample.offset);
        // What the water is doing here, scaled by how much the object couples to it. This is the
        // term that carries a raft downstream — the specification's "Current carries a floating
        // object" — and it is the SAME velocity a particle, an audio emitter and the AI read.
        const Vec3 relative = point_velocity - (here.velocity * params.current_coupling);
        const Vec3 drag = relative * (-params.linear_drag * volume);

        const Vec3 force = lift + drag;
        result.displacement_force = result.displacement_force + lift;
        result.force = result.force + force;
        // Torque about the body's origin. A single centre sample has offset zero and produces NO
        // torque, which is exactly why the specification asks for several: this cross product is
        // what makes a long vessel pitch with the swell instead of translating rigidly.
        result.torque = result.torque + cross(sample.offset, force);
    }

    if (result.submerged_samples > 0) {
        result.water_velocity = velocity_sum / static_cast<f32>(result.submerged_samples);
        // Angular drag is applied once over the whole submerged volume rather than per sample: it
        // models the hull's resistance to rotating through water, which is a property of the hull
        // and not of how finely it was sampled.
        result.torque = result.torque -
                        (state.angular_velocity * (params.angular_drag * result.submerged_volume));
    }
    return result;
}

}  // namespace cy::water
