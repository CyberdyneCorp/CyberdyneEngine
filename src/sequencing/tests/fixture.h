#pragma once
// What the suites share: an allocator, a registry of adapters with real declared properties, a
// property host that records what was captured and restored, and the small builders that make an
// authored sequence readable in a test.
//
// Deliberately not a framework. Every function here is three lines and does exactly what its name
// says, because a test whose subject is hidden behind a fixture is a test nobody can read the
// failure of.

#include <cy/core/memory/system_allocator.h>
#include <cy/sequencing/adapters.h>
#include <cy/sequencing/compile.h>
#include <cy/sequencing/source.h>

#include <utility>

namespace cy::sequencing::testing {

[[nodiscard]] inline Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A host that records every capture and restore, so "only the properties the sequence touches
/// SHALL be captured" is a count rather than a claim.
class RecordingHost final : public PropertyHost {
public:
    explicit RecordingHost(Allocator& alloc) noexcept : captured(alloc), restored(alloc) {}

    Status capture(u64 target, ResolvedProperty property, ChannelValue& out) noexcept override {
        out = current;
        (void)target;
        (void)property;
        return captured.push_back(property.property);
    }
    Status restore(u64 target, ResolvedProperty property,
                   const ChannelValue& value) noexcept override {
        (void)target;
        current = value;
        return restored.push_back(property.property);
    }

    ChannelValue current = ChannelValue::scalar(1.0F);
    Array<u32> captured;
    Array<u32> restored;
};

/// Property indices, so a test can name what it drove. They are the order they are declared in
/// below, which is what `AdapterRegistry::declare_property` returns.
enum : u32 { kIntensity = 0, kColor = 1 };

/// Register one adapter per subsystem a suite drives, with the properties each declares.
struct Registry {
    explicit Registry(Allocator& alloc) noexcept : registry(alloc), host(alloc) {}

    [[nodiscard]] Status build(bool light_can_restore = true, bool light_deterministic = true,
                               bool light_network_safe = true) noexcept {
        AdapterProperties light;
        light.name = Name::intern("light");
        light.subsystem = SubsystemId::Light;
        light.supports_capture_restore = light_can_restore;
        light.deterministic = light_deterministic;
        light.network_safe = light_network_safe;
        const Expected<u32, Error> light_id = registry.register_adapter(light, &host);
        if (!light_id) {
            return Status{make_unexpected(light_id.error())};
        }
        if (!registry.declare_property(light_id.value(), Name::intern("intensity"),
                                       ChannelType::Scalar)) {
            return fail(ErrorCode::Internal, "intensity");
        }
        if (!registry.declare_property(light_id.value(), Name::intern("colour"),
                                       ChannelType::Color)) {
            return fail(ErrorCode::Internal, "colour");
        }

        AdapterProperties camera;
        camera.name = Name::intern("camera");
        camera.subsystem = SubsystemId::Camera;
        const Expected<u32, Error> camera_id = registry.register_adapter(camera);
        if (!camera_id) {
            return Status{make_unexpected(camera_id.error())};
        }
        if (!registry.declare_property(camera_id.value(), Name::intern("weight"),
                                       ChannelType::Scalar)) {
            return fail(ErrorCode::Internal, "weight");
        }

        AdapterProperties gameplay;
        gameplay.name = Name::intern("gameplay");
        gameplay.subsystem = SubsystemId::Gameplay;
        if (!registry.register_adapter(gameplay)) {
            return fail(ErrorCode::Internal, "gameplay");
        }

        AdapterProperties animation;
        animation.name = Name::intern("animation");
        animation.subsystem = SubsystemId::Animation;
        const Expected<u32, Error> animation_id = registry.register_adapter(animation);
        if (!animation_id) {
            return Status{make_unexpected(animation_id.error())};
        }
        if (!registry.declare_property(animation_id.value(), Name::intern("rotation"),
                                       ChannelType::Rotation)) {
            return fail(ErrorCode::Internal, "rotation");
        }
        return ok();
    }

    AdapterRegistry registry;
    RecordingHost host;
};

[[nodiscard]] inline Channel scalar_channel(const char* property, u32 stable_id) noexcept {
    Channel channel(allocator());
    channel.property = Name::intern(property);
    channel.type = ChannelType::Scalar;
    channel.stable_id = stable_id;
    return channel;
}

inline void add_key(Channel& channel, i64 frame, f32 value,
                    Interpolation interpolation = Interpolation::Linear) noexcept {
    Key key;
    key.time = SequenceTime::from_frame(frame);
    key.value[0] = value;
    key.interpolation = interpolation;
    key.stable_id = static_cast<u32>(channel.keys.size() + 1);
    (void)channel.keys.push_back(key);
}

[[nodiscard]] inline Section section_over(i64 start_frame, i64 end_frame, u32 stable_id) noexcept {
    Section section(allocator());
    section.start = SequenceTime::from_frame(start_frame);
    section.end = SequenceTime::from_frame(end_frame);
    section.stable_id = stable_id;
    return section;
}

[[nodiscard]] inline Track track_of(const char* name, TrackKind kind, u32 stable_id,
                                    u32 binding) noexcept {
    Track track(allocator());
    track.name = Name::intern(name);
    track.kind = kind;
    track.stable_id = stable_id;
    track.binding = binding;
    return track;
}

[[nodiscard]] inline BindingDeclaration binding_of(u32 stable_id, const char* name,
                                                   BindingKind kind) noexcept {
    BindingDeclaration binding;
    binding.stable_id = stable_id;
    binding.name = Name::intern(name);
    binding.kind = kind;
    return binding;
}

}  // namespace cy::sequencing::testing
