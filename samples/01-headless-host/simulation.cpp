#include "simulation.h"

#include <cy/core/base/assert.h>
#include <cy/core/reflect/control_plane.h>

// The generated metadata for the two reflected types: this is what makes `type_of<Health>()` and
// `type_id_of<Health>()` compile, and an unreflected type fail to.
#include <cy/core/reflect/demo/types.reflect.h>

#include <bit>
#include <cstdio>

namespace sample {
namespace {

using cy::f32;
using cy::u32;
using cy::u64;

/// The two components, as the scheduler knows them. A component identifier and a reflected TypeId
/// are the same number here on purpose: at M2 the ECS assigns component ids from the type registry,
/// and a sample that invented a parallel numbering would be teaching the wrong thing.
const cy::jobs::ComponentTypeId kHealth = cy::reflect::type_id_of<cy::demo::Health>().value();
const cy::jobs::ComponentTypeId kPlacement = cy::reflect::type_id_of<cy::demo::Placement>().value();

/// The manifest's field identifiers (identity/manifest.toml). Spelled once, here.
constexpr cy::reflect::FieldId kHealthMaximum{1};
constexpr cy::reflect::FieldId kHealthCurrent{2};
constexpr cy::reflect::FieldId kHealthDisplayed{3};
constexpr cy::reflect::FieldId kPlacementX{1};
constexpr cy::reflect::FieldId kPlacementY{2};
constexpr cy::reflect::FieldId kPlacementRotation{3};

/// How much one entity loses per frame. A pure function of the index, so the frame is the same on
/// every machine and in every configuration.
[[nodiscard]] f32 damage_for(u32 index) noexcept {
    return 3.0F + static_cast<f32>(index % 5U);
}

void mix(u64& hash, u32 value) noexcept {
    hash ^= value;
    hash *= 0x0000'0100'0000'01B3ull;  // FNV-1a
}

void mix(u64& hash, f32 value) noexcept {
    mix(hash, std::bit_cast<u32>(value));
}

/// Append `first` then `second` to a fixed buffer, advancing `used`, and never past `capacity`.
///
/// The clamp is the whole reason this exists. A TRUNCATED snprintf returns the length it would have
/// written, not the length it wrote, so `used += snprintf(...)` can leave `used` past `capacity` —
/// and the next call then computes `capacity - used`, which is unsigned, underflows to an enormous
/// size, and hands snprintf a buffer it believes is gigabytes long. Three systems with short names
/// cannot reach that here; the clamp is so that a fourth one with a long name truncates the line
/// instead of writing off the end of the caller's stack.
void append(char* buffer, cy::usize capacity, cy::usize& used, const char* first,
            const char* second) noexcept {
    if (used + 1 >= capacity) {
        return;
    }
    const int written = std::snprintf(buffer + used, capacity - used, "%s%s", first, second);
    if (written <= 0) {
        return;
    }
    used += static_cast<cy::usize>(written);
    if (used >= capacity) {
        used = capacity - 1;
    }
}

}  // namespace

cy::Status World::resize(u32 count) noexcept {
    if (cy::Status status = health.resize(count); !status) {
        return status;
    }
    return placement.resize(count);
}

cy::Expected<Bindings, cy::Error> Bindings::resolve(
    const cy::reflect::TypeRegistry& registry) noexcept {
    const cy::reflect::TypeInfo* health =
        registry.find(cy::reflect::type_id_of<cy::demo::Health>());
    const cy::reflect::TypeInfo* placement =
        registry.find(cy::reflect::type_id_of<cy::demo::Placement>());
    if (health == nullptr || placement == nullptr) {
        return cy::fail(cy::ErrorCode::NotFound,
                        "the reflected types this sample binds to are not registered");
    }

    // One reflected lookup per binding, and the last one any of them performs. Each also checks
    // that the field really holds an f32, so a field that changed type upstream is an error here
    // rather than a reinterpretation at the first frame.
    Bindings bindings;
    struct Binding {
        const cy::reflect::TypeInfo* type = nullptr;
        cy::reflect::FieldId id;
        cy::reflect::TypedAccessor<f32>* out = nullptr;
    };
    const Binding table[] = {
        {health, kHealthMaximum, &bindings.maximum},
        {health, kHealthCurrent, &bindings.current},
        {health, kHealthDisplayed, &bindings.displayed},
        {placement, kPlacementX, &bindings.x},
        {placement, kPlacementY, &bindings.y},
        {placement, kPlacementRotation, &bindings.rotation},
    };
    for (const Binding& binding : table) {
        auto resolved = cy::reflect::resolve_field<f32>(*binding.type, binding.id);
        if (!resolved) {
            return cy::make_unexpected(resolved.error());
        }
        *binding.out = resolved.value();
    }
    return bindings;
}

// --- The systems ---------------------------------------------------------------------------------
//
// Each body is the same shape: declare the region, check the declaration once, then loop over
// entities through accessors. No reflected lookup appears below the loop, and the hot-region
// counter is what proves it in every configuration rather than only where assertions are compiled.

void Stage::decay(const cy::jobs::SystemContext& context, void* user) noexcept {
    auto& state = *static_cast<State*>(user);
    CY_ASSERT_DECLARED_ACCESS(context.guard, cy::jobs::AccessDomain::Component, kHealth,
                              cy::jobs::Access::Write);
    CY_REFLECT_HOT_REGION("headless-host.decay");

    const Bindings& fields = *state.bindings;
    const u32 count = state.world->count();
    for (u32 index = 0; index < count; ++index) {
        void* entity = &state.world->health[index];
        f32& current = fields.current(entity);
        if (current <= 0.0F) {
            continue;
        }
        current -= damage_for(index);
        if (current <= 0.0F) {
            current = 0.0F;
            // A system running in parallel may not destroy an entity; it records the intent, and
            // the stage's flush point applies it in commit order.
            (void)context.commands->destroy_entity(index);
        }
        // The presentation value follows the real one. Derived, transient, and never serialized —
        // which is what its Persistence(Derived) and Transient attributes say.
        fields.displayed(entity) += (current - fields.displayed(entity)) * 0.5F;
    }
}

void Stage::drift(const cy::jobs::SystemContext& context, void* user) noexcept {
    auto& state = *static_cast<State*>(user);
    CY_ASSERT_DECLARED_ACCESS(context.guard, cy::jobs::AccessDomain::Component, kPlacement,
                              cy::jobs::Access::Write);
    CY_REFLECT_HOT_REGION("headless-host.drift");

    const Bindings& fields = *state.bindings;
    const u32 count = state.world->count();
    for (u32 index = 0; index < count; ++index) {
        void* entity = &state.world->placement[index];
        f32& rotation = fields.rotation(entity);
        rotation += 0.0625F;
        fields.x(entity) += rotation * 0.001F;
        fields.y(entity) -= rotation * 0.001F;
    }
}

void Stage::summarise(const cy::jobs::SystemContext& context, void* user) noexcept {
    auto& state = *static_cast<State*>(user);
    CY_ASSERT_DECLARED_ACCESS(context.guard, cy::jobs::AccessDomain::Component, kHealth,
                              cy::jobs::Access::Read);
    CY_REFLECT_HOT_REGION("headless-host.summarise");

    // It reads both components, which is why the schedule puts it after both writers. It computes
    // and discards: what is being demonstrated is the ordering the declarations produced, and a
    // reader that also wrote something would blur that.
    const Bindings& fields = *state.bindings;
    const u32 count = state.world->count();
    f32 total = 0.0F;
    for (u32 index = 0; index < count; ++index) {
        total +=
            fields.current(&state.world->health[index]) + fields.x(&state.world->placement[index]);
    }
    (void)total;
}

void Stage::apply_command(const cy::jobs::StructuralCommand& command, void* user) noexcept {
    auto& retired = *static_cast<u64*>(user);
    if (command.op == cy::jobs::StructuralOp::DestroyEntity) {
        ++retired;
    }
}

// --- The stage -----------------------------------------------------------------------------------

cy::Status Stage::build(World& world, const Bindings& bindings) noexcept {
    state_.world = &world;
    state_.bindings = &bindings;

    // One command slot per entity: a frame in which every remaining entity reaches zero is the
    // worst case, and a store that refused a command would lose work silently.
    if (cy::Status ready = commands_.initialize(world.count() + 1); !ready) {
        return ready;
    }

    struct Declaration {
        const char* name;
        cy::jobs::SystemBody body;
        cy::jobs::ComponentTypeId write;
        cy::jobs::ComponentTypeId read;  // zero for none
    };
    const Declaration declarations[] = {
        {"decay", &Stage::decay, kHealth, 0},
        {"drift", &Stage::drift, kPlacement, 0},
        {"summarise", &Stage::summarise, 0, kHealth},
    };

    for (const Declaration& declaration : declarations) {
        cy::jobs::SystemDesc desc;
        desc.name = declaration.name;
        desc.body = declaration.body;
        desc.user = &state_;
        if (declaration.write != 0) {
            if (cy::Status declared = desc.access.write(declaration.write); !declared) {
                return declared;
            }
        }
        if (declaration.read != 0) {
            if (cy::Status declared = desc.access.read(declaration.read); !declared) {
                return declared;
            }
            // `summarise` also reads what `drift` writes, which is the second edge that puts it in
            // its own batch. Declared separately so the two reads read as two facts.
            if (cy::Status declared = desc.access.read(kPlacement); !declared) {
                return declared;
            }
        }
        if (auto added = schedule_.add(desc); !added) {
            return cy::make_unexpected(added.error());
        }
    }
    return schedule_.build();
}

cy::Expected<FrameResult, cy::Error> Stage::run(cy::jobs::JobSystem& jobs) noexcept {
    const u64 before = retired_;
    if (cy::Status ran = schedule_.run(jobs, &commands_, &Stage::apply_command, &retired_); !ran) {
        return cy::make_unexpected(ran.error());
    }
    ++state_.frame;
    return FrameResult{retired_ - before, retired_};
}

void Stage::format_plan(char* buffer, cy::usize capacity) const noexcept {
    if (capacity == 0) {
        return;
    }
    buffer[0] = '\0';
    cy::usize used = 0;
    for (u32 batch = 0; batch < schedule_.batch_count(); ++batch) {
        append(buffer, capacity, used, batch == 0 ? "" : " ", "[");
        for (u32 index = 0; index < schedule_.batch_size(batch); ++index) {
            append(buffer, capacity, used, index == 0 ? "" : " ",
                   schedule_.name_of(schedule_.batch_member(batch, index)));
        }
        append(buffer, capacity, used, "", "]");
    }
}

u64 Stage::checksum() const noexcept {
    u64 hash = 0xCBF2'9CE4'8422'2325ull;  // FNV-1a offset basis
    const Bindings& fields = *state_.bindings;
    const u32 count = state_.world->count();
    for (u32 index = 0; index < count; ++index) {
        const void* health = &state_.world->health[index];
        const void* placement = &state_.world->placement[index];
        mix(hash, fields.current(health));
        mix(hash, fields.displayed(health));
        mix(hash, fields.maximum(health));
        mix(hash, fields.x(placement));
        mix(hash, fields.y(placement));
        mix(hash, fields.rotation(placement));
    }
    mix(hash, static_cast<u32>(retired_));
    return hash;
}

}  // namespace sample
