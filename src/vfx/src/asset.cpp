// The VFX asset model and the CyberGraph node library. M8.c task 2.1.

#include <cy/vfx/asset.h>

#include <cy/vfx/interfaces.h>
#include <cy/vfx/ir.h>

#include <string>
#include <utility>

namespace cy::vfx {
namespace {

using graph::Capability;
using graph::Determinism;
using graph::NodeTypeDesc;
using graph::PinDesc;
using graph::PinDirection;

constexpr const char* kPlugin = "cy.vfx";

}  // namespace

const char* stage_name(Stage stage) noexcept {
    switch (stage) {
        case Stage::Spawn:
            return "Spawn";
        case Stage::Initialise:
            return "Initialise";
        case Stage::Update:
            return "Update";
        case Stage::Event:
            return "Event";
        case Stage::Render:
            return "Render";
        case Stage::Compute:
            return "Compute";
        case Stage::Count:
            break;
    }
    return "?";
}

const char* importance_name(ImportanceClass importance) noexcept {
    switch (importance) {
        case ImportanceClass::Critical:
            return "Critical";
        case ImportanceClass::Important:
            return "Important";
        case ImportanceClass::Ambient:
            return "Ambient";
        case ImportanceClass::Decorative:
            return "Decorative";
        case ImportanceClass::Count:
            break;
    }
    return "?";
}

const char* path_name(SimulationPath path) noexcept {
    return path == SimulationPath::CpuRequired ? "CpuRequired" : "GpuPreferred";
}

const char* asset_renderer_name(u8 kind) noexcept {
    constexpr const char* names[kAssetRendererCount] = {"Sprite", "Mesh", "Ribbon", "Beam",
                                                        "Trail", "Decal", "Light", "Volume"};
    return kind < kAssetRendererCount ? names[kind] : "unknown";
}

const char* precision_name(Precision precision) noexcept {
    switch (precision) {
        case Precision::Auto:
            return "Auto";
        case Precision::Float32:
            return "Float32";
        case Precision::Float16:
            return "Float16";
        case Precision::Unorm8:
            return "Unorm8";
        case Precision::Snorm16:
            return "Snorm16";
    }
    return "?";
}

u32 precision_bytes(Precision precision) noexcept {
    switch (precision) {
        case Precision::Float32:
            return 4;
        case Precision::Float16:
        case Precision::Snorm16:
            return 2;
        case Precision::Unorm8:
            return 1;
        case Precision::Auto:
            break;
    }
    return 0;
}

// --- Emitter -------------------------------------------------------------------------------------

Emitter::Emitter(Allocator& allocator, Name emitter_name) noexcept
    : name_(emitter_name),
      stages_(allocator),
      attributes_(allocator),
      interfaces_(allocator),
      modules_(allocator) {}

Status Emitter::set_stage(Stage which, Graph&& graph) noexcept {
    if (which == Stage::Count) {
        return fail(ErrorCode::InvalidArgument, "vfx: `Stage::Count` is not a stage");
    }
    for (StageEntry& entry : stages_) {
        if (entry.stage == which) {
            entry.graph = std::move(graph);
            return ok();
        }
    }
    StageEntry entry{which, std::move(graph)};
    return stages_.push_back(std::move(entry));
}

const Graph* Emitter::stage(Stage which) const noexcept {
    for (const StageEntry& entry : stages_) {
        if (entry.stage == which) {
            return &entry.graph;
        }
    }
    return nullptr;
}

Graph* Emitter::stage(Stage which) noexcept {
    for (StageEntry& entry : stages_) {
        if (entry.stage == which) {
            return &entry.graph;
        }
    }
    return nullptr;
}

void Emitter::resolve(const NodeRegistry& registry) noexcept {
    for (StageEntry& entry : stages_) {
        entry.graph.resolve(registry);
    }
}

Status Emitter::declare_attribute(const AttributeDecl& decl) noexcept {
    if (decl.name.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "vfx: an attribute declaration needs a name");
    }
    for (AttributeDecl& existing : attributes_) {
        if (existing.name == decl.name) {
            existing = decl;
            return ok();
        }
    }
    return attributes_.push_back(decl);
}

const AttributeDecl* Emitter::find_attribute(Name attribute) const noexcept {
    for (const AttributeDecl& decl : attributes_) {
        if (decl.name == attribute) {
            return &decl;
        }
    }
    return nullptr;
}

Status Emitter::bind_interface(Name interface_name) noexcept {
    if (interface_name.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "vfx: an interface binding needs a name");
    }
    for (Name existing : interfaces_) {
        if (existing == interface_name) {
            return fail(ErrorCode::InvalidArgument, "vfx: duplicate interface binding");
        }
    }
    return interfaces_.push_back(interface_name);
}

Status Emitter::reference_module(Name module_name) noexcept {
    if (module_name.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "vfx: a module reference needs a name");
    }
    for (Name existing : modules_) {
        if (existing == module_name) {
            return fail(ErrorCode::AlreadyExists, "vfx: duplicate module reference");
        }
    }
    return modules_.push_back(module_name);
}

// --- VfxSystemAsset ------------------------------------------------------------------------------

VfxSystemAsset::VfxSystemAsset(Allocator& allocator, Name asset_name) noexcept
    : name_(asset_name),
      emitters_(allocator),
      parameters_(allocator),
      channels_(allocator),
      module_assets_(allocator) {}

void VfxSystemAsset::resolve(const NodeRegistry& registry) noexcept {
    for (Emitter& emitter : emitters_) {
        emitter.resolve(registry);
    }
}

Status VfxSystemAsset::add_emitter(Emitter&& emitter) noexcept {
    if (find_emitter(emitter.name()) != nullptr) {
        return fail(
            ErrorCode::AlreadyExists,
            "vfx: two emitters of one system share a name — every event reference and every "
            "attribute reference between emitters is by name");
    }
    return emitters_.push_back(std::move(emitter));
}

const Emitter* VfxSystemAsset::find_emitter(Name emitter) const noexcept {
    for (const Emitter& candidate : emitters_) {
        if (candidate.name() == emitter) {
            return &candidate;
        }
    }
    return nullptr;
}

Status VfxSystemAsset::declare_parameter(const ParameterDecl& decl) noexcept {
    if (decl.name.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "vfx: a parameter needs a name");
    }
    if (vfx_type_from_name(decl.type) == graph::kInvalidType) {
        return fail(ErrorCode::InvalidArgument,
                    "vfx: a parameter's type is not in the VFX type lattice");
    }
    for (ParameterDecl& existing : parameters_) {
        if (existing.name == decl.name) {
            existing = decl;
            return ok();
        }
    }
    return parameters_.push_back(decl);
}

const ParameterDecl* VfxSystemAsset::find_parameter(Name parameter) const noexcept {
    for (const ParameterDecl& decl : parameters_) {
        if (decl.name == parameter) {
            return &decl;
        }
    }
    return nullptr;
}

Status VfxSystemAsset::declare_channel(const EventChannelDecl& decl) noexcept {
    if (decl.name.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "vfx: an event channel needs a name");
    }
    if (decl.max_events_per_frame == 0 || decl.max_chain_depth == 0) {
        // BOTH BOUNDS ARE REQUIRED, so a channel cannot be declared without them. `vfx-system`:
        // "Every event channel SHALL declare a maximum events per frame and every chain a maximum
        // depth" — a zero here is a channel whose feedback loop has nothing stopping it.
        return fail(ErrorCode::InvalidArgument,
                    "vfx: an event channel declares a maximum events per frame and a maximum chain "
                    "depth, and neither may be zero");
    }
    for (EventChannelDecl& existing : channels_) {
        if (existing.name == decl.name) {
            existing = decl;
            return ok();
        }
    }
    return channels_.push_back(decl);
}

const EventChannelDecl* VfxSystemAsset::find_channel(Name channel) const noexcept {
    for (const EventChannelDecl& decl : channels_) {
        if (decl.name == channel) {
            return &decl;
        }
    }
    return nullptr;
}

Status VfxSystemAsset::declare_module_asset(const ModuleAssetRef& reference) noexcept {
    if (reference.name.is_empty() || reference.path.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "vfx: a module asset needs a name and path");
    }
    for (const ModuleAssetRef& existing : module_assets_) {
        if (existing.name == reference.name || existing.path == reference.path) {
            return fail(ErrorCode::AlreadyExists, "vfx: duplicate module asset mapping");
        }
    }
    return module_assets_.push_back(reference);
}

const ModuleAssetRef* VfxSystemAsset::find_module_asset(Name name) const noexcept {
    for (const ModuleAssetRef& reference : module_assets_) {
        if (reference.name == name) {
            return &reference;
        }
    }
    return nullptr;
}

// --- The node library ----------------------------------------------------------------------------

namespace {

/// One row of the library. The pins are built at registration because `PinDesc` holds `Name`s and a
/// `Name` cannot be interned in a constant expression.
struct NodeRow {
    graph::NodeTypeId identity;
    const char* type;
    /// Input pin names and their types, `nullptr`-terminated pairs.
    const char* const* inputs;
    const char* output_type;
    Determinism determinism;
    bool pure;
    Capability required;
};

constexpr const char* kNone[] = {nullptr};
constexpr const char* kUnary[] = {"x", "float", nullptr};
constexpr const char* kBinary[] = {"a", "float", "b", "float", nullptr};
constexpr const char* kTernary[] = {"a", "float", "b", "float", "t", "float", nullptr};
constexpr const char* kSelect[] = {"condition", "bool", "a", "float", "b", "float", nullptr};
constexpr const char* kValueOnly[] = {"value", "float", nullptr};
constexpr const char* kPredicate[] = {"value", "bool", nullptr};
constexpr const char* kMake3[] = {"x", "float", "y", "float", "z", "float", nullptr};
constexpr const char* kMake4[] = {"x", "float", "y", "float", "z", "float", "w", "float", nullptr};

constexpr NodeRow kLibrary[] = {
    {1001, "vfx.constant", kNone, "float", Determinism::Deterministic, true, Capability::None},
    {1002, "vfx.parameter", kNone, "float", Determinism::Deterministic, true, Capability::None},
    {1003, "vfx.attribute", kNone, "float", Determinism::Deterministic, true, Capability::None},
    {1004, "vfx.input", kNone, "float", Determinism::Deterministic, true, Capability::None},
    {1005, "vfx.random", kNone, "float", Determinism::SeedDependent, true, Capability::Randomness},
    {1006, "vfx.sample", kUnary, "float", Determinism::Deterministic, true, Capability::ReadWorld},
    {1007, "vfx.curve", kUnary, "float", Determinism::Deterministic, true, Capability::None},
    {1008, "vfx.noise", kUnary, "float", Determinism::Deterministic, true, Capability::None},
    {1009, "vfx.add", kBinary, "float", Determinism::Deterministic, true, Capability::None},
    {1010, "vfx.sub", kBinary, "float", Determinism::Deterministic, true, Capability::None},
    {1011, "vfx.mul", kBinary, "float", Determinism::Deterministic, true, Capability::None},
    {1012, "vfx.div", kBinary, "float", Determinism::Deterministic, true, Capability::None},
    {1013, "vfx.min", kBinary, "float", Determinism::Deterministic, true, Capability::None},
    {1014, "vfx.max", kBinary, "float", Determinism::Deterministic, true, Capability::None},
    {1015, "vfx.dot", kBinary, "float", Determinism::Deterministic, true, Capability::None},
    {1016, "vfx.cross", kBinary, "float3", Determinism::Deterministic, true, Capability::None},
    {1017, "vfx.length", kUnary, "float", Determinism::Deterministic, true, Capability::None},
    {1018, "vfx.normalize", kUnary, "float", Determinism::Deterministic, true, Capability::None},
    {1019, "vfx.sin", kUnary, "float", Determinism::Deterministic, true, Capability::None},
    {1020, "vfx.cos", kUnary, "float", Determinism::Deterministic, true, Capability::None},
    {1021, "vfx.pow", kBinary, "float", Determinism::Deterministic, true, Capability::None},
    {1022, "vfx.saturate", kUnary, "float", Determinism::Deterministic, true, Capability::None},
    {1023, "vfx.lerp", kTernary, "float", Determinism::Deterministic, true, Capability::None},
    {1024, "vfx.select", kSelect, "float", Determinism::Deterministic, true, Capability::None},
    {1025, "vfx.less", kBinary, "bool", Determinism::Deterministic, true, Capability::None},
    {1026, "vfx.greater", kBinary, "bool", Determinism::Deterministic, true, Capability::None},
    {1027, "vfx.make_float3", kMake3, "float3", Determinism::Deterministic, true, Capability::None},
    {1028, "vfx.make_float4", kMake4, "float4", Determinism::Deterministic, true, Capability::None},
    // THE THREE NODES THAT ARE NOT PURE, and the reason this domain has its own IR. Each one is a
    // WRITE — of an attribute, of the kill predicate, of the spawn count — and a hash-consed
    // expression DAG has no Store. They become entries in the kernel's ordered write list.
    {1029, "vfx.set_attribute", kValueOnly, nullptr, Determinism::Deterministic, false,
     Capability::None},
    {1030, "vfx.kill_if", kPredicate, nullptr, Determinism::Deterministic, false, Capability::None},
    {1031, "vfx.spawn_count", kValueOnly, nullptr, Determinism::Deterministic, false,
     Capability::None},
    // A GPU-TO-GPU EVENT RAISE. Not pure, like the three above: it is a write into an event
    // channel, and `vfx-system` requires that channel to carry both of its bounds.
    {1032, "vfx.emit_event", kPredicate, nullptr, Determinism::Deterministic, false,
     Capability::None},
};

[[nodiscard]] Status register_row(NodeRegistry& registry, const NodeRow& row,
                                  Array<PinDesc>& pins) noexcept {
    pins.clear();
    for (const char* const* cursor = row.inputs; *cursor != nullptr; cursor += 2) {
        PinDesc pin;
        pin.identity = static_cast<graph::PinId>(pins.size() + 1);
        pin.name = Name::intern(cursor[0]);
        pin.type = Name::intern(cursor[1]);
        pin.direction = PinDirection::Input;
        pin.required = true;
        if (Status pushed = pins.push_back(pin); !pushed) {
            return pushed;
        }
    }
    if (row.output_type != nullptr) {
        PinDesc pin;
        pin.identity = static_cast<graph::PinId>(pins.size() + 1);
        pin.name = Name::intern("out");
        pin.type = Name::intern(row.output_type);
        pin.direction = PinDirection::Output;
        if (Status pushed = pins.push_back(pin); !pushed) {
            return pushed;
        }
    }

    NodeTypeDesc desc;
    desc.identity = row.identity;
    desc.name = Name::intern(row.type);
    desc.plugin = Name::intern(kPlugin);
    desc.version = 1;
    desc.pins = pins.span();
    desc.requires_capabilities = row.required;
    desc.determinism = row.determinism;
    desc.pure = row.pure;
    return registry.register_type(desc);
}

[[nodiscard]] graph::NodeTypeId sample_identity(std::string_view name) noexcept {
    u32 hash = 2166136261U;
    for (const char character : name) {
        hash = (hash ^ static_cast<u8>(character)) * 16777619U;
    }
    return 0x80000000U | (hash & 0x7fffffffU);
}

}  // namespace

Status register_vfx_interface_nodes(NodeRegistry& registry,
                                    const DataInterfaceRegistry& interfaces) noexcept {
    for (const DataInterface& interface : interfaces.all()) {
        for (const InterfaceField& field : interface.fields()) {
            const std::string name = std::string("vfx.sample.") +
                                     std::string(interface.name().text()) + "." + field.name;
            PinDesc pins[2];
            pins[0].identity = 1;
            pins[0].name = Name::intern("x");
            pins[0].type = Name::intern("float");
            pins[0].direction = PinDirection::Input;
            pins[0].required = true;
            pins[1].identity = 2;
            pins[1].name = Name::intern("out");
            pins[1].type = Name::intern(vfx_type_name(field.type));
            pins[1].direction = PinDirection::Output;
            NodeTypeDesc desc;
            desc.identity = sample_identity(name);
            desc.name = Name::intern(name);
            desc.plugin = Name::intern(kPlugin);
            desc.version = interface.version();
            desc.pins = {pins, 2};
            desc.requires_capabilities = Capability::ReadWorld;
            if (const graph::NodeType* existing = registry.find(desc.name); existing != nullptr) {
                if (existing->identity() == desc.identity && existing->version() == desc.version &&
                    existing->pins().size() == 2 && existing->pins()[1].type == pins[1].type) {
                    continue;
                }
                return fail(ErrorCode::AlreadyExists,
                            "a VFX interface field changed without a new catalogue identity");
            }
            if (Status registered = registry.register_type(desc); !registered) {
                return registered;
            }
        }
    }
    return ok();
}

Status register_vfx_nodes(NodeRegistry& registry) noexcept {
    Array<PinDesc> pins(registry.allocator());
    for (const NodeRow& row : kLibrary) {
        if (Status registered = register_row(registry, row, pins); !registered) {
            return registered;
        }
    }
    // Every VFX pin type converts to every other numeric one: a graph is authored by an artist
    // dragging a float into a float3 input, and the lowering broadcasts. Declaring the conversions
    // is what makes `cy::graph::validate` agree with the lowering rather than refuse what it
    // accepts.
    // `bool` is in the list because a predicate and a number are interchangeable to an artist:
    // `kill_if` takes a bool and a `greater` node produces one, but wiring a float straight into a
    // predicate is what an author expects to work. The lowering broadcasts either way, and
    // declaring the conversion is what keeps `cy::graph::validate` agreeing with it.
    static constexpr const char* kNumeric[] = {"float",  "float2", "float3",
                                               "float4", "int",    "bool"};
    for (const char* from : kNumeric) {
        for (const char* to : kNumeric) {
            if (Status allowed = registry.allow_conversion(Name::intern(from), Name::intern(to));
                !allowed) {
                return allowed;
            }
        }
    }
    DataInterfaceRegistry interfaces(registry.allocator());
    if (Status registered = register_builtin_interfaces(interfaces); !registered) {
        return registered;
    }
    return register_vfx_interface_nodes(registry, interfaces);
}

}  // namespace cy::vfx
