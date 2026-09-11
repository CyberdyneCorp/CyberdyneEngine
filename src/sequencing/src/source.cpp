// The authored model's spellings, its one derived table, and the transform helper.

#include <cy/sequencing/source.h>

#include <utility>

namespace cy::sequencing {

const char* subsystem_name(SubsystemId subsystem) noexcept {
    switch (subsystem) {
        case SubsystemId::Camera:
            return "Camera";
        case SubsystemId::Animation:
            return "Animation";
        case SubsystemId::Audio:
            return "Audio";
        case SubsystemId::Effects:
            return "Effects";
        case SubsystemId::Material:
            return "Material";
        case SubsystemId::Light:
            return "Light";
        case SubsystemId::Environment:
            return "Environment";
        case SubsystemId::Interface:
            return "Interface";
        case SubsystemId::Gameplay:
            return "Gameplay";
        case SubsystemId::Count:
            break;
    }
    return "Unknown";
}

const char* track_kind_name(TrackKind kind) noexcept {
    switch (kind) {
        case TrackKind::Property:
            return "Property";
        case TrackKind::Transform:
            return "Transform";
        case TrackKind::Animation:
            return "Animation";
        case TrackKind::Camera:
            return "Camera";
        case TrackKind::CameraCut:
            return "CameraCut";
        case TrackKind::Audio:
            return "Audio";
        case TrackKind::Effects:
            return "Effects";
        case TrackKind::Material:
            return "Material";
        case TrackKind::Light:
            return "Light";
        case TrackKind::Environment:
            return "Environment";
        case TrackKind::GameplayEvent:
            return "GameplayEvent";
        case TrackKind::GameplayCommand:
            return "GameplayCommand";
        case TrackKind::Interface:
            return "Interface";
        case TrackKind::WorldLayer:
            return "WorldLayer";
        case TrackKind::TimeScale:
            return "TimeScale";
        case TrackKind::NestedSequence:
            return "NestedSequence";
        case TrackKind::Marker:
            return "Marker";
        case TrackKind::Count:
            break;
    }
    return "Unknown";
}

SubsystemId subsystem_for(TrackKind kind) noexcept {
    switch (kind) {
        case TrackKind::Camera:
        case TrackKind::CameraCut:
            return SubsystemId::Camera;
        case TrackKind::Animation:
            return SubsystemId::Animation;
        case TrackKind::Audio:
            return SubsystemId::Audio;
        case TrackKind::Effects:
            return SubsystemId::Effects;
        case TrackKind::Material:
            return SubsystemId::Material;
        case TrackKind::Light:
            return SubsystemId::Light;
        case TrackKind::Environment:
        case TrackKind::WorldLayer:
            return SubsystemId::Environment;
        case TrackKind::Interface:
            return SubsystemId::Interface;
        case TrackKind::GameplayEvent:
        case TrackKind::GameplayCommand:
            return SubsystemId::Gameplay;
        // A property, a transform, a time scale, a nested sequence and a marker are not a
        // subsystem's: a property track's subsystem is decided by the ADAPTER its property resolves
        // against, and the other three are the sequence's own bookkeeping. `Animation` is the
        // default because a property track's ordinary target is a posed thing; the compiler
        // resolves the real one and says so when it cannot.
        case TrackKind::Property:
        case TrackKind::Transform:
        case TrackKind::TimeScale:
        case TrackKind::NestedSequence:
        case TrackKind::Marker:
        case TrackKind::Count:
            break;
    }
    return SubsystemId::Animation;
}

const char* authority_class_name(AuthorityClass authority) noexcept {
    switch (authority) {
        case AuthorityClass::PresentationOnly:
            return "PresentationOnly";
        case AuthorityClass::LocalGameplay:
            return "LocalGameplay";
        case AuthorityClass::AuthoritativeGameplay:
            return "AuthoritativeGameplay";
        case AuthorityClass::DeterministicSimulation:
            return "DeterministicSimulation";
        case AuthorityClass::Count:
            break;
    }
    return "Unknown";
}

const char* channel_type_name(ChannelType type) noexcept {
    switch (type) {
        case ChannelType::Scalar:
            return "Scalar";
        case ChannelType::Vector:
            return "Vector";
        case ChannelType::Rotation:
            return "Rotation";
        case ChannelType::Color:
            return "Color";
        case ChannelType::Boolean:
            return "Boolean";
        case ChannelType::Enumeration:
            return "Enumeration";
        case ChannelType::Transform:
            return "Transform";
        case ChannelType::Count:
            break;
    }
    return "Unknown";
}

u8 channel_component_count(ChannelType type) noexcept {
    switch (type) {
        case ChannelType::Scalar:
            return 1;
        case ChannelType::Vector:
            return 3;
        case ChannelType::Rotation:
        case ChannelType::Color:
            return 4;
        case ChannelType::Boolean:
        case ChannelType::Enumeration:
            return 0;  // carried in `integer`
        case ChannelType::Transform:
        case ChannelType::Count:
            break;
    }
    return 0;
}

const char* interpolation_name(Interpolation interpolation) noexcept {
    switch (interpolation) {
        case Interpolation::Constant:
            return "Constant";
        case Interpolation::Linear:
            return "Linear";
        case Interpolation::Smooth:
            return "Smooth";
        case Interpolation::Count:
            break;
    }
    return "Unknown";
}

const char* completion_policy_name(CompletionPolicy policy) noexcept {
    switch (policy) {
        case CompletionPolicy::Restore:
            return "Restore";
        case CompletionPolicy::HoldFinal:
            return "HoldFinal";
        case CompletionPolicy::KeepPermanently:
            return "KeepPermanently";
        case CompletionPolicy::Custom:
            return "Custom";
        case CompletionPolicy::Count:
            break;
    }
    return "Unknown";
}

const char* blend_mode_name(BlendMode mode) noexcept {
    switch (mode) {
        case BlendMode::Absolute:
            return "Absolute";
        case BlendMode::Additive:
            return "Additive";
        case BlendMode::Override:
            return "Override";
        case BlendMode::Count:
            break;
    }
    return "Unknown";
}

const char* side_effect_policy_name(SideEffectPolicy policy) noexcept {
    switch (policy) {
        case SideEffectPolicy::Reversible:
            return "Reversible";
        case SideEffectPolicy::Idempotent:
            return "Idempotent";
        case SideEffectPolicy::Speculative:
            return "Speculative";
        case SideEffectPolicy::ConfirmedOnly:
            return "ConfirmedOnly";
        case SideEffectPolicy::Count:
            break;
    }
    return "Unknown";
}

const char* binding_kind_name(BindingKind kind) noexcept {
    switch (kind) {
        case BindingKind::Entity:
            return "Entity";
        case BindingKind::Participant:
            return "Participant";
        case BindingKind::Camera:
            return "Camera";
        case BindingKind::Interface:
            return "Interface";
        case BindingKind::Service:
            return "Service";
        case BindingKind::WorldLayer:
            return "WorldLayer";
        case BindingKind::AudioBus:
            return "AudioBus";
        case BindingKind::Project:
            return "Project";
        case BindingKind::Count:
            break;
    }
    return "Unknown";
}

const char* skip_policy_name(SkipPolicy policy) noexcept {
    switch (policy) {
        case SkipPolicy::NotSkippable:
            return "NotSkippable";
        case SkipPolicy::PresentationOnly:
            return "PresentationOnly";
        case SkipPolicy::ApplyRequiredOutcomes:
            return "ApplyRequiredOutcomes";
        case SkipPolicy::Custom:
            return "Custom";
        case SkipPolicy::Count:
            break;
    }
    return "Unknown";
}

const char* network_policy_name(NetworkPolicy policy) noexcept {
    switch (policy) {
        case NetworkPolicy::LocalOnly:
            return "LocalOnly";
        case NetworkPolicy::ServerTriggered:
            return "ServerTriggered";
        case NetworkPolicy::Synchronised:
            return "Synchronised";
        case NetworkPolicy::Deterministic:
            return "Deterministic";
        case NetworkPolicy::Count:
            break;
    }
    return "Unknown";
}

SubsystemId track_subsystem(const Track& track) noexcept {
    return (track.subsystem == SubsystemId::Count) ? subsystem_for(track.kind) : track.subsystem;
}

ChannelValue ChannelValue::scalar(f32 value) noexcept {
    ChannelValue result;
    result.type = ChannelType::Scalar;
    result.components[0] = value;
    return result;
}

ChannelValue ChannelValue::vector(f32 x, f32 y, f32 z) noexcept {
    ChannelValue result;
    result.type = ChannelType::Vector;
    result.components[0] = x;
    result.components[1] = y;
    result.components[2] = z;
    return result;
}

ChannelValue ChannelValue::rotation(f32 x, f32 y, f32 z, f32 w) noexcept {
    ChannelValue result;
    result.type = ChannelType::Rotation;
    result.components[0] = x;
    result.components[1] = y;
    result.components[2] = z;
    result.components[3] = w;
    return result;
}

ChannelValue ChannelValue::color(f32 r, f32 g, f32 b, f32 a) noexcept {
    ChannelValue result;
    result.type = ChannelType::Color;
    result.components[0] = r;
    result.components[1] = g;
    result.components[2] = b;
    result.components[3] = a;
    return result;
}

ChannelValue ChannelValue::boolean(bool value) noexcept {
    ChannelValue result;
    result.type = ChannelType::Boolean;
    result.integer = value ? 1U : 0U;
    return result;
}

ChannelValue ChannelValue::enumeration(u32 value) noexcept {
    ChannelValue result;
    result.type = ChannelType::Enumeration;
    result.integer = value;
    return result;
}

bool ChannelValue::equals(const ChannelValue& other) const noexcept {
    if (type != other.type) {
        return false;
    }
    if (integer != other.integer) {
        return false;
    }
    const u8 count = channel_component_count(type);
    for (u8 index = 0; index < count; ++index) {
        if (components[index] != other.components[index]) {
            return false;
        }
    }
    return true;
}

Status add_transform_channels(Section& section, u32 first_stable_id) noexcept {
    // The three channels a transform is, each with the interpolation its own type requires. A
    // rotation authored as three Euler scalars would interpolate component-wise, which is the
    // defect "Rotation interpolates as rotation" is written to forbid.
    struct Declaration {
        const char* name;
        ChannelType type;
    };
    const Declaration declarations[3] = {{"position", ChannelType::Vector},
                                         {"rotation", ChannelType::Rotation},
                                         {"scale", ChannelType::Vector}};
    for (u32 index = 0; index < 3; ++index) {
        Channel channel(section.channels.allocator());
        channel.property = Name::intern(declarations[index].name);
        channel.type = declarations[index].type;
        channel.stable_id = first_stable_id + index;
        if (Status pushed = section.channels.push_back(std::move(channel)); !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace cy::sequencing
