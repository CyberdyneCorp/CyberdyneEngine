#pragma once
// Track adapters: what a track kind can promise, and where a property name stops being a string.
// M8.c tasks 3.3 and 3.5.
//
// ================================================================================================
// AN ADAPTER DECLARES ITS PROPERTIES SO THE COMPILER CAN REFUSE A TRACK
// ================================================================================================
//
// `sequencing-and-cinematics` — "Track extension": a custom track "SHALL **declare its
// properties**: thread safety, determinism, seekability, reversibility, network safety, and whether
// it is editor-only. The compiler SHALL validate its use against the sequence's domain,
// determinism profile, and network policy." And — "Seeking and scrubbing" — "Each track adapter
// SHALL declare its **seek capability**".
//
// Those declarations are not advisory here. `compile.h` reads every field of `AdapterProperties`
// and fails the compilation naming the track and the property that disqualified it, which is the
// requirement's own scenario. An adapter that lied about being deterministic would be a defect in
// the adapter; an adapter whose declaration nothing read would be a defect in this design.
//
// ================================================================================================
// AND IT DECLARES ITS PROPERTY NAMES SO EVALUATION NEVER SEES ONE
// ================================================================================================
//
// A channel is authored against a name — "intensity", "position", "opacity". `resolve()` turns that
// into a `ResolvedProperty` of two small integers AT COMPILE TIME, and the runtime carries the
// integers. That is "string paths SHALL NOT be looked up during evaluation", enforced by the
// compiled form not having a string in it.
//
// ================================================================================================
// CAPTURE AND RESTORE BELONG TO THE ADAPTER, NOT TO THE SEQUENCE
// ================================================================================================
//
// "Where restoration is required, the adapter for that property SHALL provide **capture and
// restore**, and only the properties the sequence touches SHALL be captured. An arbitrary object
// snapshot SHALL NOT be taken." `PropertyHost` is that pair of calls, and it is addressed by a
// resolved property and a resolved target — so what is captured is exactly what a channel drives.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/sequencing/program.h>
#include <cy/sequencing/source.h>

namespace cy::sequencing {

/// How an adapter can reach an arbitrary time. "Capabilities differ legitimately — a pose can be
/// evaluated at a time, a particle system may need to simulate, an audio stream may need to seek or
/// restart."
enum class SeekCapability : u8 {
    /// Evaluate directly at any time. The only capability for which play and seek are required to
    /// agree, and `player.h` tests exactly that pair.
    Evaluate = 0,
    Reconstruct,
    SimulateWithPreRoll,
    Restart,
    Count,
};

[[nodiscard]] const char* seek_capability_name(SeekCapability capability) noexcept;

/// What an adapter promises. Every field is read by the compiler; see the header comment.
struct AdapterProperties {
    Name name;
    SubsystemId subsystem = SubsystemId::Camera;
    /// May its batch be filled from a worker?
    bool thread_safe = true;
    /// Does it produce the same result from the same inputs on every machine? A deterministic
    /// sequence may use no adapter that says false.
    bool deterministic = true;
    /// May a sequence under a network policy other than `LocalOnly` drive it?
    bool network_safe = true;
    /// Can its contribution be evaluated in reverse?
    bool reversible = true;
    /// Editor-only adapters are refused in a cooked sequence.
    bool editor_only = false;
    /// Does it implement `PropertyHost`? A section declaring `CompletionPolicy::Restore` against an
    /// adapter that does not is a compile error rather than a value that never comes back.
    bool supports_capture_restore = false;
    SeekCapability seek = SeekCapability::Evaluate;
};

/// The runtime half of an adapter: capture and restore, addressed by resolved identities.
///
/// Deliberately NOT a "dispatch" interface. "Tracks SHALL NOT invoke subsystems directly during
/// traversal" — evaluation fills batches, and a subsystem consumes its batch at its own point. What
/// the sequence calls directly is only this pair, and only at a section boundary.
class PropertyHost {
public:
    PropertyHost() = default;
    virtual ~PropertyHost() = default;
    PropertyHost(const PropertyHost&) = delete;
    PropertyHost& operator=(const PropertyHost&) = delete;

    /// `target` is the resolved binding target — whatever `BindingResolution::target` carried.
    [[nodiscard]] virtual Status capture(u64 target, ResolvedProperty property,
                                         ChannelValue& out) noexcept = 0;
    [[nodiscard]] virtual Status restore(u64 target, ResolvedProperty property,
                                         const ChannelValue& value) noexcept = 0;
};

/// The adapters a project has registered, and the properties each one exposes.
///
/// Consulted at COMPILE time. It is deliberately not consulted per frame: everything it would be
/// asked has already been turned into an integer in the program.
class AdapterRegistry {
public:
    explicit AdapterRegistry(Allocator& allocator) noexcept;

    /// Register an adapter. The returned id is what `ResolvedProperty::adapter` carries; it is
    /// one-based so that a zeroed `ResolvedProperty` is recognisably unresolved.
    [[nodiscard]] Expected<u32, Error> register_adapter(const AdapterProperties& properties,
                                                        PropertyHost* host = nullptr) noexcept;

    /// Declare a property this adapter drives. Returns its index within the adapter.
    [[nodiscard]] Expected<u32, Error> declare_property(u32 adapter, Name property,
                                                        ChannelType type) noexcept;

    [[nodiscard]] const AdapterProperties* properties(u32 adapter) const noexcept;
    [[nodiscard]] PropertyHost* host(u32 adapter) const noexcept;
    [[nodiscard]] u32 adapter_count() const noexcept { return static_cast<u32>(adapters_.size()); }

    /// The adapter registered for a subsystem, or zero. One per subsystem: a second registration
    /// for the same subsystem is an error rather than a silent replacement, because a project that
    /// registered two would have no way to tell which one a track reached.
    [[nodiscard]] u32 adapter_for(SubsystemId subsystem) const noexcept;

    /// Resolve a property name against the adapter for a subsystem. The compiler's only lookup.
    [[nodiscard]] Expected<ResolvedProperty, Error> resolve(SubsystemId subsystem, Name property,
                                                            ChannelType type) const noexcept;

    /// The type a property was declared with, for the compiler's type check.
    [[nodiscard]] ChannelType property_type(ResolvedProperty property) const noexcept;
    [[nodiscard]] Name property_name(ResolvedProperty property) const noexcept;

private:
    struct Record {
        AdapterProperties properties;
        PropertyHost* host = nullptr;
        u32 property_begin = 0;
        u32 property_count = 0;
    };
    struct PropertyRecord {
        Name name;
        ChannelType type = ChannelType::Scalar;
    };

    Array<Record> adapters_;
    Array<PropertyRecord> properties_;
};

}  // namespace cy::sequencing
