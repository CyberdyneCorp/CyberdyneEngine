// The determinism firewall's enforcement point. M8.c tasks 1.1 and 1.2.
//
// THE DIAGNOSTIC IS EMITTED HERE AND NOT LEFT FOR A CALLER TO READ. `vfx-system`: "Development
// builds SHALL detect and report attempts to write replicated or physics-owned components from
// VFX-driven code paths", and M8.c's delta adds "not a diagnostic that a caller may choose to
// read". So a refusal writes a log record and a trace instant at the moment it happens, naming the
// writer, the component and the path — the three things the requirement lists — and the retained
// `FirewallViolation` is a convenience for a test rather than the report itself.
//
// `cy::core-diagnostics` is a PRIVATE dependency of this module, which is why the trace appears in
// this file and not in firewall.h: including a component header must not drag the trace in behind
// it. Same rule, same reason, as src/ecs/src/diagnostics.cpp.
//
// THE CLASSIFICATIONS ARE Public. A writer name, a component name and a path name are compiled
// identifiers — the same category as the component and system names the ECS already emits.

#include <cy/ecs/firewall.h>

#include <cy/core/determinism/classification.h>
#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/trace.h>
#include <cy/core/reflect/attributes.h>
#include <cy/core/reflect/type_info.h>

namespace cy::ecs {
namespace {

// The whole reporting apparatus is inside `CY_DEVELOPMENT`, and so are the declarations it needs.
// `vfx-system` asks for the report in development builds; a Shipping build still REFUSES and still
// counts — that is the firewall — it just does not carry the names. Left outside the guard, these
// declarations are a `-Wunused-function` error in Shipping, which is the build catching exactly the
// thing it should: code compiled into a shipping binary that nothing can reach.
#if defined(CY_DEVELOPMENT)
CY_TRACE_CATEGORY(category, "ecs")
CY_LOG_CATEGORY(log_category, "ecs.firewall")

CY_TRACE_FIELD(writer_name, string, cy::Privacy::Public)
CY_TRACE_FIELD(origin_name, string, cy::Privacy::Public)
CY_TRACE_FIELD(path_name, string, cy::Privacy::Public)
CY_TRACE_FIELD(guarded_component, string, cy::Privacy::Public)
CY_TRACE_FIELD(authority_name, string, cy::Privacy::Public)
CY_TRACE_FIELD(entity_index, id, cy::Privacy::Public)
CY_TRACE_FIELD(refusal_ordinal, u64, cy::Privacy::Public)
#endif

#if defined(CY_DEVELOPMENT)
[[nodiscard]] diag::FieldValue text_field(diag::FieldId field, const char* text) noexcept {
    u32 length = 0;
    while (text != nullptr && text[length] != '\0' && length < diag::kMaxTextBytesPerRecord) {
        ++length;
    }
    return diag::field_text(field, (text == nullptr) ? "" : text, length);
}
#endif

/// The frame the calling thread is running under. A thread that never opened a `WriteScope` is at
/// `Simulation`, which is what makes adopting the firewall a change to VFX and inference rather
/// than to every system in the engine.
struct OriginFrame {
    WriteOrigin origin = WriteOrigin::Simulation;
    const char* writer = "simulation";
};

thread_local OriginFrame t_frame;

/// What one reflected component derives, or `Presentation` when it derives nothing. See
/// `WriteFirewall::declare_from_reflection` for why an undeclared `Persistence` derives nothing.
[[nodiscard]] ComponentAuthority derive_authority(const reflect::TypeInfo& type) noexcept {
    ComponentAuthority derived = ComponentAuthority::Presentation;
    for (u32 index = 0; index < type.field_count; ++index) {
        const reflect::FieldAttributes& attributes = type.fields[index].attributes;
        if (attributes.declares(reflect::AttributeKind::Replicated)) {
            return ComponentAuthority::Replicated;
        }
        if (attributes.declares(reflect::AttributeKind::Persistence) &&
            determinism::is_authoritative(determinism::class_of(attributes.persistence))) {
            derived = ComponentAuthority::Authoritative;
        }
    }
    return derived;
}

}  // namespace

const char* write_origin_name(WriteOrigin origin) noexcept {
    switch (origin) {
        case WriteOrigin::Simulation:
            return "simulation";
        case WriteOrigin::PinnedInference:
            return "pinned-inference";
        case WriteOrigin::Vfx:
            return "vfx";
        case WriteOrigin::Inference:
            return "inference";
        case WriteOrigin::Presentation:
            return "presentation";
        case WriteOrigin::Count:
            break;
    }
    return "unknown";
}

const char* component_authority_name(ComponentAuthority authority) noexcept {
    switch (authority) {
        case ComponentAuthority::Presentation:
            return "presentation";
        case ComponentAuthority::Authoritative:
            return "authoritative";
        case ComponentAuthority::Replicated:
            return "replicated";
        case ComponentAuthority::PhysicsOwned:
            return "physics-owned";
        case ComponentAuthority::Count:
            break;
    }
    return "unknown";
}

const char* write_path_name(WritePath path) noexcept {
    switch (path) {
        case WritePath::GetMut:
            return "World::get_mut";
        case WritePath::QueryWrite:
            return "QueryChunk::write";
        case WritePath::Structural:
            return "World::add/remove/set_shared";
        case WritePath::SparseWrite:
            return "World::set_sparse";
        case WritePath::SparseRemove:
            return "World::remove_sparse";
        case WritePath::EntityLifetime:
            return "World::create/destroy/instantiate";
        case WritePath::Relationship:
            return "World::set_parent";
        case WritePath::DeferredRecord:
            return "CommandBuffer::record";
        case WritePath::Count:
            break;
    }
    return "unknown";
}

WriteOrigin current_write_origin() noexcept {
    return t_frame.origin;
}

const char* current_writer() noexcept {
    return t_frame.writer;
}

WriteScope::WriteScope(WriteOrigin origin, const char* writer) noexcept
    : previous_origin_(t_frame.origin), previous_writer_(t_frame.writer) {
    t_frame.origin = origin;
    t_frame.writer = (writer == nullptr) ? "unnamed" : writer;
}

WriteScope::~WriteScope() {
    t_frame.origin = previous_origin_;
    t_frame.writer = previous_writer_;
}

// --- Declaration --------------------------------------------------------------------------------

Status WriteFirewall::declare(ComponentTypeId component, ComponentAuthority authority) noexcept {
    if (component >= kMaxComponentTypes) {
        return fail(ErrorCode::InvalidArgument,
                    "declare() names a component id outside this world's range");
    }
    if (authority >= ComponentAuthority::Count) {
        return fail(ErrorCode::InvalidArgument, "declare() names no such authority");
    }
    const ComponentAuthority current = authority_[component];
    if (current == authority) {
        return ok();
    }
    if (authority_is_guarded(current) && !authority_is_guarded(authority)) {
        // Lowering is refused rather than obeyed: a component that physics declared its own does
        // not become presentation because a second caller registered it more loosely, and a
        // firewall whose strength depends on registration order is not one.
        return fail(ErrorCode::InvalidArgument,
                    "a guarded component cannot be redeclared as presentation");
    }
    if (!authority_is_guarded(current) && authority_is_guarded(authority)) {
        ++guarded_count_;
    }
    authority_[component] = authority;
    return ok();
}

Status WriteFirewall::declare_from_reflection(const ComponentRegistry& registry,
                                              AuthorityDerivationReport& report) noexcept {
    report = AuthorityDerivationReport{};
    for (ComponentTypeId component = 0; component < registry.size(); ++component) {
        ++report.components_examined;
        if (authority_is_guarded(authority_of(component))) {
            ++report.already_declared;
            continue;
        }
        const ComponentInfo& info = registry.info(component);
        if (info.type == nullptr) {
            // A built-in registered by name: no descriptor to derive from. `Parent` is guarded by
            // the relationship path instead, which refuses re-parenting outright.
            ++report.underived;
            continue;
        }
        const ComponentAuthority derived = derive_authority(*info.type);
        if (!authority_is_guarded(derived)) {
            ++report.underived;
            continue;
        }
        if (Status declared = declare(component, derived); !declared) {
            return declared;
        }
        if (derived == ComponentAuthority::Replicated) {
            ++report.guarded_by_replication;
        } else {
            ++report.guarded_by_persistence;
        }
    }
    return ok();
}

// --- The check ----------------------------------------------------------------------------------

bool WriteFirewall::admit(WritePath path, ComponentTypeId component, Entity entity,
                          const ComponentRegistry& registry) noexcept {
    const WriteOrigin origin = current_write_origin();
    if (origin_may_write_authoritative(origin) || !armed_) {
        return true;
    }
    const bool lifetime = path == WritePath::EntityLifetime || path == WritePath::Relationship;
    const ComponentAuthority authority = authority_of(component);
    if (!lifetime && !authority_is_guarded(authority)) {
        return true;
    }
    record(origin, path, component, authority, entity, registry);
    return false;
}

void WriteFirewall::record(WriteOrigin origin, WritePath path, ComponentTypeId component,
                           ComponentAuthority authority, Entity entity,
                           const ComponentRegistry& registry) noexcept {
    const u64 ordinal = refusals_.fetch_add(1, std::memory_order_relaxed) + 1;
    by_origin_[static_cast<u32>(origin)].fetch_add(1, std::memory_order_relaxed);

    const char* component_name =
        registry.registered(component) ? registry.info(component).name : "<none>";

    FirewallViolation violation;
    violation.origin = origin;
    violation.writer = current_writer();
    violation.path = path;
    violation.component = component;
    violation.component_name = component_name;
    violation.authority = authority;
    violation.entity = entity;
    violation.ordinal = ordinal;
    violations_[(ordinal - 1) % kMaxRecordedViolations] = violation;

#if defined(CY_DEVELOPMENT)
    // The report the requirement asks for, at the moment of the refusal. Error rather than Warning:
    // a VFX or inference code path reaching authoritative state is a determinism defect, and the
    // consequence — a desync months later — is what this exists to stop being the way it is found.
    CY_LOG(log_category(), diag::LogLevel::Error, "ecs.firewall.refused",
           text_field(writer_name(), violation.writer),
           text_field(origin_name(), write_origin_name(origin)),
           text_field(path_name(), write_path_name(path)),
           text_field(guarded_component(), component_name),
           text_field(authority_name(), component_authority_name(authority)),
           diag::field_u64(entity_index(), entity.index()),
           diag::field_u64(refusal_ordinal(), ordinal));
    CY_TRACE_INSTANT("ecs.firewall.refused", category(), diag::Channel::Important,
                     text_field(writer_name(), violation.writer),
                     text_field(origin_name(), write_origin_name(origin)),
                     text_field(path_name(), write_path_name(path)),
                     text_field(guarded_component(), component_name),
                     text_field(authority_name(), component_authority_name(authority)),
                     diag::field_u64(entity_index(), entity.index()),
                     diag::field_u64(refusal_ordinal(), ordinal));
#endif
}

// --- Arming and inspection ----------------------------------------------------------------------

void WriteFirewall::disarm_for_negative_control(const char* reason) noexcept {
    armed_ = false;
    disarm_reason_ = (reason == nullptr) ? "unstated" : reason;
}

void WriteFirewall::rearm() noexcept {
    armed_ = true;
    disarm_reason_ = "";
}

u32 WriteFirewall::violation_count() const noexcept {
    const u64 total = refusals();
    return static_cast<u32>((total < kMaxRecordedViolations) ? total : kMaxRecordedViolations);
}

FirewallViolation WriteFirewall::last_violation() const noexcept {
    const u64 total = refusals();
    if (total == 0) {
        return FirewallViolation{};
    }
    return violations_[(total - 1) % kMaxRecordedViolations];
}

void WriteFirewall::clear_violations() noexcept {
    refusals_.store(0, std::memory_order_relaxed);
    for (std::atomic<u64>& counter : by_origin_) {
        counter.store(0, std::memory_order_relaxed);
    }
    for (FirewallViolation& violation : violations_) {
        violation = FirewallViolation{};
    }
}

}  // namespace cy::ecs
