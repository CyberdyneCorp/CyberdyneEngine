// SPDX-License-Identifier: MIT
#include <cy/vfx/catalogue.h>

#include <cy/vfx/asset.h>
#include <cy/vfx/interfaces.h>

#include <iterator>
#include <string_view>

namespace cy::vfx {
namespace {

Status put_u32(Array<u8>& out, u32 value) noexcept {
    for (usize byte = 0; byte < 4; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8)) & 0xffU));
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status put_u64(Array<u8>& out, u64 value) noexcept {
    for (usize byte = 0; byte < 8; ++byte) {
        if (Status pushed = out.push_back(static_cast<u8>((value >> (byte * 8)) & 0xffU));
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status put_text(Array<u8>& out, std::string_view value) noexcept {
    if (Status length = put_u32(out, static_cast<u32>(value.size())); !length) {
        return length;
    }
    return out.append({reinterpret_cast<const u8*>(value.data()), value.size()});
}

struct PropertyDesc {
    u32 identity;
    u8 kind;  // Text=0, Scalar=2, Enumeration=4 in the shared catalogue schema.
    std::string_view name;
    std::string_view fallback;
    std::string_view tooltip;
    std::string_view semantic;
    std::string_view choices;
    bool interface_choices = false;
};

constexpr PropertyDesc kConstant[] = {
    {1, 0, "value", "0", "One to four space-separated finite components", "vfx-literal", {}}};
constexpr PropertyDesc kParameter[] = {
    {1, 0, "parameter", {}, "Declared system parameter name", "identifier", {}}};
constexpr PropertyDesc kAttribute[] = {
    {1, 0, "attribute", {}, "Declared emitter attribute name", "identifier", {}}};
constexpr PropertyDesc kInput[] = {{1, 4, "input", "dt", "Emitter or particle input", "input",
                                    "dt|emitter_age|particle_index|spawn_index|normalised_age"}};
constexpr PropertyDesc kSample[] = {
    {1, 4, "interface", {}, "Registered engine data interface", "data-interface", {}, true},
    {2, 0, "field", {}, "Field declared by the selected interface", "identifier", {}}};
constexpr PropertyDesc kCurve[] = {
    {1, 0, "curve", {}, "Curve asset or named noise source", "identifier", {}}};
constexpr PropertyDesc kEvent[] = {
    {1, 0, "channel", {}, "Declared event channel name", "identifier", {}}};

// THE COLLIDERS' OPTIONAL INPUTS, as values an author can type. Each is the input of the same name
// left unwired: `compile.cpp` reads a wire first, then this property, then the default shown here.
// The response comes first on every collider, the shape's own inputs after it.
constexpr PropertyDesc kCollideResponse[] = {
    {1, 0, "radius", "0", "Particle radius in metres", "vfx-literal", {}},
    {2, 0, "restitution", "0.5", "Normal speed kept by a bounce, 0 to 1", "vfx-literal", {}},
    {3, 0, "friction", "0", "Coulomb friction coefficient against the surface", "vfx-literal", {}},
    {4, 0, "stick", "0", "Above one half, a particle stops dead on contact", "vfx-literal", {}},
    {5, 0, "rest_speed", "0", "Closing speeds below this do not bounce", "vfx-literal", {}}};
constexpr PropertyDesc kCollidePlane[] = {
    {6, 0, "normal", "0 1 0", "Plane normal, effect space", "vfx-literal", {}},
    {7, 0, "offset", "0", "Plane offset along its normal", "vfx-literal", {}}};
constexpr PropertyDesc kCollideSphere[] = {
    {6, 0, "centre", "0 0 0", "Sphere centre, effect space", "vfx-literal", {}},
    {7, 0, "sphere_radius", "1", "Sphere radius in metres", "vfx-literal", {}}};
constexpr PropertyDesc kCollideBox[] = {
    {6, 0, "centre", "0 0 0", "Box centre, effect space", "vfx-literal", {}},
    {7, 0, "half_extents", "0.5 0.5 0.5", "Axis-aligned half extents", "vfx-literal", {}}};
constexpr PropertyDesc kCollideCapsule[] = {
    {6, 0, "start", "0 0 0", "First end of the axis, effect space", "vfx-literal", {}},
    {7, 0, "end", "0 1 0", "Second end of the axis, effect space", "vfx-literal", {}},
    {8, 0, "capsule_radius", "0.5", "Capsule radius in metres", "vfx-literal", {}}};
constexpr PropertyDesc kCollideSdf[] = {
    {6, 0, "step", "0", "Gradient sample step in metres; 0 is 5 cm", "vfx-literal", {}}};
constexpr PropertyDesc kCollideDepth[] = {{6,
                                           0,
                                           "thickness",
                                           "0",
                                           "How deep a surface is behind its depth; 0 is 50 cm",
                                           "vfx-literal",
                                           {}}};

/// A node's properties: the ones every node of its kind shares, then its own.
struct PropertyLists {
    Span<const PropertyDesc> shared;
    Span<const PropertyDesc> own;
};

[[nodiscard]] Span<const PropertyDesc> collide_shape_properties(std::string_view name) noexcept {
    if (name == "vfx.collide.plane") {
        return {kCollidePlane, std::size(kCollidePlane)};
    }
    if (name == "vfx.collide.sphere") {
        return {kCollideSphere, std::size(kCollideSphere)};
    }
    if (name == "vfx.collide.box") {
        return {kCollideBox, std::size(kCollideBox)};
    }
    if (name == "vfx.collide.capsule") {
        return {kCollideCapsule, std::size(kCollideCapsule)};
    }
    if (name == "vfx.collide.sdf") {
        return {kCollideSdf, std::size(kCollideSdf)};
    }
    if (name == "vfx.collide.depth") {
        return {kCollideDepth, std::size(kCollideDepth)};
    }
    return {};
}

Span<const PropertyDesc> properties_for(std::string_view name) noexcept {
    if (name == "vfx.constant") {
        return {kConstant, std::size(kConstant)};
    }
    if (name == "vfx.parameter") {
        return {kParameter, std::size(kParameter)};
    }
    if (name == "vfx.attribute" || name == "vfx.set_attribute") {
        return {kAttribute, std::size(kAttribute)};
    }
    if (name == "vfx.input") {
        return {kInput, std::size(kInput)};
    }
    if (name == "vfx.sample") {
        return {kSample, std::size(kSample)};
    }
    if (name == "vfx.curve" || name == "vfx.noise") {
        return {kCurve, std::size(kCurve)};
    }
    if (name == "vfx.emit_event" || name == "vfx.event_count" || name == "vfx.event_payload") {
        return {kEvent, std::size(kEvent)};
    }
    return {};
}

Status put_choices(Array<u8>& out, const PropertyDesc& property,
                   const DataInterfaceRegistry& interfaces) noexcept {
    if (property.interface_choices) {
        if (Status count = put_u32(out, static_cast<u32>(interfaces.size())); !count) {
            return count;
        }
        for (const DataInterface& interface : interfaces.all()) {
            if (Status name = put_text(out, interface.name().text()); !name) {
                return name;
            }
        }
        return ok();
    }
    if (property.choices.empty()) {
        return put_u32(out, 0);
    }
    u32 count = 1;
    for (const char value : property.choices) {
        count += value == '|' ? 1U : 0U;
    }
    if (Status written = put_u32(out, count); !written) {
        return written;
    }
    std::string_view remaining = property.choices;
    while (!remaining.empty()) {
        const usize split = remaining.find('|');
        if (Status written = put_text(out, remaining.substr(0, split)); !written) {
            return written;
        }
        remaining.remove_prefix(split == std::string_view::npos ? remaining.size() : split + 1);
    }
    return ok();
}

Status put_property_identity(Array<u8>& out, const PropertyDesc& property) noexcept {
    if (Status value = put_u32(out, property.identity); !value) {
        return value;
    }
    if (Status value = out.push_back(property.kind); !value) {
        return value;
    }
    if (Status value = put_text(out, property.name); !value) {
        return value;
    }
    return put_text(out, property.fallback);
}

Status put_property_metadata(Array<u8>& out, const PropertyDesc& property,
                             const DataInterfaceRegistry& interfaces,
                             graph::Capability required) noexcept {
    if (Status value = put_text(out, property.tooltip); !value) {
        return value;
    }
    if (Status value = put_text(out, property.semantic); !value) {
        return value;
    }
    if (Status value = put_text(out, {}); !value) {
        return value;  // asset kind
    }
    if (Status value = put_choices(out, property, interfaces); !value) {
        return value;
    }
    if (Status value = put_text(out, "compile"); !value) {
        return value;
    }
    if (Status value = put_text(out, "vfx"); !value) {
        return value;
    }
    if (Status value = put_u64(out, static_cast<u64>(required)); !value) {
        return value;
    }
    if (Status value = out.push_back(0); !value) {
        return value;  // vector lanes
    }
    if (Status value = out.push_back(0); !value) {
        return value;  // numeric constraints
    }
    for (u32 unused = 0; unused < 3; ++unused) {
        if (Status value = put_u64(out, 0); !value) {
            return value;
        }
    }
    return ok();
}

Status put_property(Array<u8>& out, const PropertyDesc& property,
                    const DataInterfaceRegistry& interfaces, graph::Capability required) noexcept {
    if (Status identity = put_property_identity(out, property); !identity) {
        return identity;
    }
    return put_property_metadata(out, property, interfaces, required);
}

Status put_node(Array<u8>& out, const graph::NodeType& node,
                const DataInterfaceRegistry& interfaces) noexcept {
    if (Status value = put_u32(out, node.identity()); !value) {
        return value;
    }
    if (Status value = put_u32(out, node.version()); !value) {
        return value;
    }
    if (Status value = put_text(out, node.name().text()); !value) {
        return value;
    }
    if (Status value = put_u32(out, static_cast<u32>(node.pins().size())); !value) {
        return value;
    }
    for (const graph::PinDesc& pin : node.pins()) {
        if (Status value = put_u32(out, pin.identity); !value) {
            return value;
        }
        if (Status value = out.push_back(static_cast<u8>(pin.direction)); !value) {
            return value;
        }
        if (Status value = put_text(out, pin.name.text()); !value) {
            return value;
        }
        if (Status value = put_text(out, pin.type.text()); !value) {
            return value;
        }
    }
    const std::string_view name = node.name().text();
    const PropertyLists lists = name.starts_with("vfx.collide.")
                                    ? PropertyLists{{kCollideResponse, std::size(kCollideResponse)},
                                                    collide_shape_properties(name)}
                                    : PropertyLists{{}, properties_for(name)};
    if (Status count = put_u32(out, static_cast<u32>(lists.shared.size() + lists.own.size()));
        !count) {
        return count;
    }
    for (const Span<const PropertyDesc> list : {lists.shared, lists.own}) {
        for (const PropertyDesc& property : list) {
            if (Status written = put_property(out, property, interfaces, node.required());
                !written) {
                return written;
            }
        }
    }
    return ok();
}

}  // namespace

Status encode_vfx_catalogue(Array<u8>& out) noexcept {
    graph::NodeRegistry registry(out.allocator());
    if (Status registered = register_vfx_nodes(registry); !registered) {
        return registered;
    }
    DataInterfaceRegistry interfaces(out.allocator());
    if (Status registered = register_builtin_interfaces(interfaces); !registered) {
        return registered;
    }
    out.clear();
    if (Status value = put_u32(out, 2); !value) {
        return value;  // schema
    }
    if (Status value = put_u32(out, 2); !value) {
        return value;  // catalogue version
    }
    if (Status value = put_u32(out, static_cast<u32>(registry.size())); !value) {
        return value;
    }
    for (const graph::NodeType& node : registry.types()) {
        if (Status value = put_node(out, node, interfaces); !value) {
            return value;
        }
    }
    return ok();
}

}  // namespace cy::vfx
