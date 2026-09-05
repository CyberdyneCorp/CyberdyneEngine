// Processor evaluation and the mapping context's binding list. Task 4.1.3.
//
// `evaluate_processors` is the whole numerical pipeline and it is deliberately one function with a
// switch rather than a set of virtual `Processor` objects. `input-and-actions`: "Processors SHALL
// be stateless where possible; those requiring state SHALL store it in per-binding storage rather
// than as allocated objects", and "Processor evaluation SHALL NOT allocate per frame". A chain of
// polymorphic objects would have violated both — one allocation per processor per binding, and a
// vtable pointer in a struct the cook wants to write as bytes.

#include <cy/servers/input/binding.h>

#include <cy/core/base/assert.h>
#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::input {
namespace {

[[nodiscard]] f32 magnitude(Vec3 value) noexcept {
    return std::sqrt((value.x * value.x) + (value.y * value.y) + (value.z * value.z));
}

/// Per-axis dead zone with rescaling. The rescaling is the part that is usually missing: without
/// it, a stick with a 0.2 dead zone reaches only 0.8 at full deflection and the character never
/// runs at full speed.
[[nodiscard]] f32 dead_zone_axis(f32 value, f32 threshold) noexcept {
    const f32 size = std::fabs(value);
    if (size <= threshold) {
        return 0.0F;
    }
    if (threshold >= 1.0F) {
        return value;
    }
    const f32 rescaled = (size - threshold) / (1.0F - threshold);
    return value < 0.0F ? -rescaled : rescaled;
}

[[nodiscard]] Vec3 apply_dead_zone(Vec3 value, f32 threshold) noexcept {
    return Vec3{dead_zone_axis(value.x, threshold), dead_zone_axis(value.y, threshold),
                dead_zone_axis(value.z, threshold)};
}

/// Radial dead zone: the threshold applies to the vector, not to each axis.
///
/// The difference matters and is visible in play. An axis-wise dead zone leaves a *square* hole, so
/// a stick pushed diagonally at 0.15 on each axis registers while the same stick pushed straight up
/// at 0.15 does not — the character drifts diagonally and nowhere else.
[[nodiscard]] Vec3 apply_radial_dead_zone(Vec3 value, f32 threshold) noexcept {
    const f32 size = magnitude(value);
    if (size <= threshold) {
        return Vec3{};
    }
    if (size <= 0.0F || threshold >= 1.0F) {
        return value;
    }
    const f32 rescaled = (size - threshold) / (1.0F - threshold);
    const f32 factor = rescaled / size;
    return Vec3{value.x * factor, value.y * factor, value.z * factor};
}

[[nodiscard]] Vec3 apply_normalize(Vec3 value) noexcept {
    const f32 size = magnitude(value);
    if (size <= 1.0F || size <= 0.0F) {
        return value;
    }
    return Vec3{value.x / size, value.y / size, value.z / size};
}

[[nodiscard]] f32 response_curve(f32 value, f32 exponent) noexcept {
    const f32 size = std::pow(std::fabs(value), exponent);
    return value < 0.0F ? -size : size;
}

/// `a` packs three 2-bit source selectors and three sign bits, cooked. Never parsed at runtime —
/// see types.h's `cook` namespace for the rule this keeps.
[[nodiscard]] Vec3 apply_swizzle(Vec3 value, f32 packed) noexcept {
    const auto bits = static_cast<u32>(packed);
    Vec3 out;
    for (u32 axis = 0; axis < 3; ++axis) {
        const u32 source = (bits >> (axis * 3U)) & 0x3U;
        const bool negate = ((bits >> ((axis * 3U) + 2U)) & 0x1U) != 0U;
        const f32 component = source < 3U ? value[source] : 0.0F;
        out[axis] = negate ? -component : component;
    }
    return out;
}

/// Exponential smoothing toward the input. `a` is the time constant in seconds; zero disables it,
/// which is what makes a chain authored with a smoothing slot at zero cost nothing.
[[nodiscard]] Vec3 apply_smooth(Vec3 value, Vec3& state, bool& primed, f32 time_constant,
                                f32 seconds) noexcept {
    if (time_constant <= 0.0F || seconds <= 0.0F) {
        state = value;
        primed = true;
        return value;
    }
    if (!primed) {
        // The first sample seeds the filter. Starting from zero would make every action ramp up
        // from nothing on the frame it is first touched, which reads as input lag.
        state = value;
        primed = true;
        return value;
    }
    const f32 alpha = 1.0F - std::exp(-seconds / time_constant);
    state = Vec3{state.x + ((value.x - state.x) * alpha), state.y + ((value.y - state.y) * alpha),
                 state.z + ((value.z - state.z) * alpha)};
    return state;
}

}  // namespace

const char* processor_kind_name(ProcessorKind kind) noexcept {
    switch (kind) {
        case ProcessorKind::None:
            return "None";
        case ProcessorKind::DeadZone:
            return "DeadZone";
        case ProcessorKind::RadialDeadZone:
            return "RadialDeadZone";
        case ProcessorKind::Normalize:
            return "Normalize";
        case ProcessorKind::Scale:
            return "Scale";
        case ProcessorKind::Invert:
            return "Invert";
        case ProcessorKind::Sensitivity:
            return "Sensitivity";
        case ProcessorKind::ResponseCurve:
            return "ResponseCurve";
        case ProcessorKind::Clamp:
            return "Clamp";
        case ProcessorKind::Swizzle:
            return "Swizzle";
        case ProcessorKind::Smooth:
            return "Smooth";
        case ProcessorKind::Count:
            break;
    }
    return "None";
}

const char* modifier_kind_name(ModifierKind kind) noexcept {
    switch (kind) {
        case ModifierKind::None:
            return "None";
        case ModifierKind::ReferenceFrame:
            return "ReferenceFrame";
        case ModifierKind::SettingScale:
            return "SettingScale";
        case ModifierKind::Negate:
            return "Negate";
        case ModifierKind::ActiveInState:
            return "ActiveInState";
        case ModifierKind::Count:
            break;
    }
    return "None";
}

const char* binding_kind_name(BindingKind kind) noexcept {
    switch (kind) {
        case BindingKind::Simple:
            return "Simple";
        case BindingKind::Axis1D:
            return "Axis1D";
        case BindingKind::Axis2D:
            return "Axis2D";
        case BindingKind::Radial:
            return "Radial";
        case BindingKind::Chord:
            return "Chord";
        case BindingKind::Sequence:
            return "Sequence";
        case BindingKind::Count:
            break;
    }
    return "Simple";
}

Vec3 evaluate_processors(const Processor* processors, u8 count, Vec3 value, ProcessorState& state,
                         f32 seconds) noexcept {
    CY_ASSERT_MSG(count <= kMaxProcessors, "a binding's chain is at most kMaxProcessors long");
    for (u8 index = 0; index < count && index < kMaxProcessors; ++index) {
        const Processor& processor = processors[index];
        switch (processor.kind) {
            case ProcessorKind::None:
                break;
            case ProcessorKind::DeadZone:
                value = apply_dead_zone(value, processor.a);
                break;
            case ProcessorKind::RadialDeadZone:
                value = apply_radial_dead_zone(value, processor.a);
                break;
            case ProcessorKind::Normalize:
                value = apply_normalize(value);
                break;
            case ProcessorKind::Scale:
                value = Vec3{value.x * processor.a,
                             value.y * (processor.b != 0.0F ? processor.b : processor.a),
                             value.z * (processor.b != 0.0F ? processor.b : processor.a)};
                break;
            case ProcessorKind::Invert:
                value = Vec3{-value.x, -value.y, -value.z};
                break;
            case ProcessorKind::Sensitivity:
                value = Vec3{value.x * processor.a, value.y * processor.a, value.z * processor.a};
                break;
            case ProcessorKind::ResponseCurve:
                value =
                    Vec3{response_curve(value.x, processor.a), response_curve(value.y, processor.a),
                         response_curve(value.z, processor.a)};
                break;
            case ProcessorKind::Clamp:
                value = Vec3{math::clamp(value.x, processor.a, processor.b),
                             math::clamp(value.y, processor.a, processor.b),
                             math::clamp(value.z, processor.a, processor.b)};
                break;
            case ProcessorKind::Swizzle:
                value = apply_swizzle(value, processor.a);
                break;
            case ProcessorKind::Smooth:
                value = apply_smooth(value, state.smoothed[index], state.primed[index], processor.a,
                                     seconds);
                break;
            case ProcessorKind::Count:
                break;
        }
    }
    return value;
}

Status MappingContext::add(const Binding& binding) noexcept {
    if (binding.action == kInvalidAction) {
        return fail(ErrorCode::InvalidArgument, "input: a binding needs an action");
    }
    if (binding.component_count == 0 || binding.component_count > kMaxComponents) {
        return fail(ErrorCode::InvalidArgument,
                    "input: a binding has between one and kMaxComponents controls");
    }
    if (binding.processor_count > kMaxProcessors) {
        return fail(ErrorCode::OutOfRange, "input: a binding's processor chain is too long");
    }
    return bindings_.push_back(binding);
}

}  // namespace cy::input
