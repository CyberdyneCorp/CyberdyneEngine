// The adapter registry: registration, property declaration, and the compiler's one lookup.

#include <cy/sequencing/adapters.h>

namespace cy::sequencing {

const char* seek_capability_name(SeekCapability capability) noexcept {
    switch (capability) {
        case SeekCapability::Evaluate:
            return "Evaluate";
        case SeekCapability::Reconstruct:
            return "Reconstruct";
        case SeekCapability::SimulateWithPreRoll:
            return "SimulateWithPreRoll";
        case SeekCapability::Restart:
            return "Restart";
        case SeekCapability::Count:
            break;
    }
    return "Unknown";
}

AdapterRegistry::AdapterRegistry(Allocator& allocator) noexcept
    : adapters_(allocator), properties_(allocator) {}

Expected<u32, Error> AdapterRegistry::register_adapter(const AdapterProperties& properties,
                                                       PropertyHost* host) noexcept {
    if (adapter_for(properties.subsystem) != 0) {
        // A second adapter for one subsystem would leave a track with no way to say which one it
        // meant, and the failure would be a value driven by the wrong thing rather than an error.
        return fail(ErrorCode::AlreadyExists, "a subsystem already has an adapter");
    }
    Record record;
    record.properties = properties;
    record.host = host;
    record.property_begin = static_cast<u32>(properties_.size());
    record.property_count = 0;
    if (Status pushed = adapters_.push_back(record); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<u32>(adapters_.size());  // one-based
}

Expected<u32, Error> AdapterRegistry::declare_property(u32 adapter, Name property,
                                                       ChannelType type) noexcept {
    if (adapter == 0 || adapter > adapters_.size()) {
        return fail(ErrorCode::NotFound, "no such adapter");
    }
    // Properties are stored contiguously per adapter, so they must be declared before the next
    // adapter is registered. Refused rather than reordered: an interleaved declaration would put a
    // property in another adapter's range, and every resolution after it would be silently wrong.
    if (adapter != adapters_.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "declare an adapter's properties before registering the next adapter");
    }
    Record& record = adapters_[adapter - 1];
    for (u32 index = 0; index < record.property_count; ++index) {
        if (properties_[record.property_begin + index].name == property) {
            return fail(ErrorCode::AlreadyExists, "the adapter already declares this property");
        }
    }
    if (Status pushed = properties_.push_back(PropertyRecord{property, type}); !pushed) {
        return make_unexpected(pushed.error());
    }
    const u32 index = record.property_count;
    ++record.property_count;
    return index;
}

const AdapterProperties* AdapterRegistry::properties(u32 adapter) const noexcept {
    if (adapter == 0 || adapter > adapters_.size()) {
        return nullptr;
    }
    return &adapters_[adapter - 1].properties;
}

PropertyHost* AdapterRegistry::host(u32 adapter) const noexcept {
    if (adapter == 0 || adapter > adapters_.size()) {
        return nullptr;
    }
    return adapters_[adapter - 1].host;
}

u32 AdapterRegistry::adapter_for(SubsystemId subsystem) const noexcept {
    for (usize index = 0; index < adapters_.size(); ++index) {
        if (adapters_[index].properties.subsystem == subsystem) {
            return static_cast<u32>(index + 1);
        }
    }
    return 0;
}

Expected<ResolvedProperty, Error> AdapterRegistry::resolve(SubsystemId subsystem, Name property,
                                                           ChannelType type) const noexcept {
    const u32 adapter = adapter_for(subsystem);
    if (adapter == 0) {
        return fail(ErrorCode::NotFound, "no adapter registered for this subsystem");
    }
    const Record& record = adapters_[adapter - 1];
    for (u32 index = 0; index < record.property_count; ++index) {
        const PropertyRecord& candidate = properties_[record.property_begin + index];
        if (candidate.name != property) {
            continue;
        }
        if (candidate.type != type) {
            return fail(ErrorCode::InvalidArgument, "the property is declared at another type");
        }
        ResolvedProperty resolved;
        resolved.adapter = adapter;
        resolved.property = index;
        return resolved;
    }
    return fail(ErrorCode::NotFound, "the adapter declares no such property");
}

ChannelType AdapterRegistry::property_type(ResolvedProperty property) const noexcept {
    if (property.adapter == 0 || property.adapter > adapters_.size()) {
        return ChannelType::Count;
    }
    const Record& record = adapters_[property.adapter - 1];
    if (property.property >= record.property_count) {
        return ChannelType::Count;
    }
    return properties_[record.property_begin + property.property].type;
}

Name AdapterRegistry::property_name(ResolvedProperty property) const noexcept {
    if (property.adapter == 0 || property.adapter > adapters_.size()) {
        return Name{};
    }
    const Record& record = adapters_[property.adapter - 1];
    if (property.property >= record.property_count) {
        return Name{};
    }
    return properties_[record.property_begin + property.property].name;
}

}  // namespace cy::sequencing
