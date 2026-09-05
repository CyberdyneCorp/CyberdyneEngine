// Distance attenuation, cones, panning, Doppler and filter-based occlusion.
// See cy/servers/audio/spatial.h.

#include <cy/servers/audio/spatial.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::audio {
namespace {

/// Every model divides by the reference distance, and a reference of zero is an authored mistake
/// rather than a silent NaN.
constexpr f32 kMinReference = 0.0001F;

}  // namespace

const char* attenuation_model_name(AttenuationModel model) noexcept {
    switch (model) {
        case AttenuationModel::Inverse:
            return "inverse";
        case AttenuationModel::InverseSquare:
            return "inverse-square";
        case AttenuationModel::Linear:
            return "linear";
        case AttenuationModel::Logarithmic:
            return "logarithmic";
        case AttenuationModel::Custom:
            return "custom";
        case AttenuationModel::Count:
            break;
    }
    return "unknown";
}

f32 attenuation_gain(const Attenuation& attenuation, f32 distance) noexcept {
    const f32 reference = math::max(attenuation.reference_distance, kMinReference);
    const f32 maximum = math::max(attenuation.max_distance, reference);
    const f32 clamped = math::clamp(distance, reference, maximum);

    // BEYOND MAX_DISTANCE IS SILENT, IN EVERY MODEL. That is what makes a voice inaudible, and
    // inaudible is what makes it virtualisable — the tier assignment reads this gain. A model that
    // approached zero without reaching it would leave every distant source in the mix forever.
    if (distance >= maximum) {
        return 0.0F;
    }

    switch (attenuation.model) {
        case AttenuationModel::Inverse:
            return reference / (reference + (attenuation.rolloff * (clamped - reference)));
        case AttenuationModel::InverseSquare: {
            const f32 ratio = reference / clamped;
            return ratio * ratio;
        }
        case AttenuationModel::Linear:
            return math::clamp((maximum - clamped) / (maximum - reference), 0.0F, 1.0F);
        case AttenuationModel::Logarithmic: {
            // Amplitude halves every doubling of distance, scaled by the rolloff. Expressed through
            // the ratio rather than through decibels so the curve is one expression rather than a
            // conversion in and a conversion out.
            const f32 ratio = clamped / reference;
            return 1.0F / std::pow(ratio, math::max(attenuation.rolloff, 0.0F));
        }
        case AttenuationModel::Custom:
        case AttenuationModel::Count:
            break;
    }

    if (attenuation.curve == nullptr) {
        // A `Custom` model with no curve is a configuration mistake. Full gain rather than silence:
        // a sound that is too loud is noticed and fixed, and one that is silent is assumed missing.
        return 1.0F;
    }
    const f32 normalised = (clamped - reference) / (maximum - reference);
    return math::clamp(attenuation.curve(normalised, attenuation.curve_user), 0.0F, 1.0F);
}

f32 cone_gain(const Cone& cone, Vec3 source_forward, Vec3 source_to_listener) noexcept {
    if (length_squared(source_forward) < 1e-8F || length_squared(source_to_listener) < 1e-8F) {
        return 1.0F;
    }
    const f32 cosine =
        math::clamp(dot(normalize(source_forward), normalize(source_to_listener)), -1.0F, 1.0F);
    const f32 angle = 2.0F * std::acos(cosine);  // the full cone angle this direction sits inside

    const f32 inner = math::max(cone.inner_angle_radians, 0.0F);
    const f32 outer = math::max(cone.outer_angle_radians, inner);
    if (angle <= inner) {
        return 1.0F;
    }
    if (angle >= outer) {
        return math::clamp(cone.outer_gain, 0.0F, 1.0F);
    }
    const f32 t = (angle - inner) / (outer - inner);
    return math::lerp(1.0F, math::clamp(cone.outer_gain, 0.0F, 1.0F), t);
}

PanGains pan_stereo(Vec3 direction_in_listener_space) noexcept {
    if (length_squared(direction_in_listener_space) < 1e-8F) {
        // At the listener's own position there is no direction. Centre, at equal power, which is
        // what a source you are standing inside should sound like.
        constexpr f32 centre = 0.70710678F;
        return PanGains{centre, centre};
    }
    const Vec3 direction = normalize(direction_in_listener_space);
    // The listener looks down its local −Z and +X is its right, so `direction.x` is the pan
    // position directly. Mapped from [−1, 1] to [0, 1] and then through a quarter turn of sine and
    // cosine, which is the constant-power law: left² + right² is one at every position.
    const f32 pan = math::clamp((direction.x + 1.0F) * 0.5F, 0.0F, 1.0F);
    const f32 angle = pan * (math::kPi * 0.5F);
    return PanGains{std::cos(angle), std::sin(angle)};
}

f32 doppler_pitch(Vec3 listener_position, Vec3 listener_velocity, Vec3 source_position,
                  Vec3 source_velocity, f32 speed_of_sound, f32 factor) noexcept {
    if (factor <= 0.0F || speed_of_sound <= 0.0F) {
        return 1.0F;
    }
    const Vec3 to_listener = listener_position - source_position;
    if (length_squared(to_listener) < 1e-8F) {
        return 1.0F;
    }
    const Vec3 direction = normalize(to_listener);

    // Closing speeds along the line between them. Positive means approaching.
    const f32 listener_speed = dot(listener_velocity, direction);
    const f32 source_speed = dot(source_velocity, direction);
    const f32 scaled = speed_of_sound / factor;

    // The classical expression, with the denominator clamped away from zero: a source moving toward
    // the listener at the speed of sound would otherwise divide by zero and a faster one would
    // produce a NEGATIVE pitch, which plays the clip backwards at an enormous rate.
    const f32 denominator = math::max(scaled - source_speed, scaled * 0.1F);
    return math::clamp((scaled - listener_speed) / denominator, 0.25F, 4.0F);
}

f32 occlusion_filter(f32 occlusion, f32 sample_rate, f32& gain_out) noexcept {
    const f32 amount = math::clamp(occlusion, 0.0F, 1.0F);
    // OCCLUSION IS BOTH A FILTER AND AN ATTENUATION. `audio`: "the source SHALL be low-pass
    // filtered
    // **and** attenuated by the occlusion parameters." Returning only the cutoff is how the second
    // half gets forgotten, so this function returns one and writes the other.
    gain_out = math::lerp(1.0F, 0.25F, amount);

    // From open (half the sample rate, which is no filtering at all) down to 400 Hz — muffled the
    // way a voice through a wall is. Interpolated geometrically rather than linearly because pitch
    // perception is logarithmic: a linear sweep spends almost all of its travel above 10 kHz, where
    // it is inaudible.
    const f32 open = math::max(sample_rate * 0.5F, 1000.0F);
    constexpr f32 closed = 400.0F;
    return open * std::pow(closed / open, amount);
}

}  // namespace cy::audio
