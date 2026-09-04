// The ECS's built-in components, declared to the state hash. See cy/ecs/state_schema.h.

#include <cy/ecs/state_schema.h>

#include <cy/ecs/entity.h>
#include <cy/ecs/relationships.h>

#include <array>
#include <bit>
#include <cstddef>

namespace cy::ecs {
namespace {

using determinism::SchemaSubject;
using determinism::SimulationClass;
using determinism::StateField;

/// The field ids are this subject's own numbering and start at 1. They are not manifest identifiers
/// and must not look like them: a built-in has none, `StateField::id` only has to be stable across
/// runs and unique within the subject, and both are properties of a literal written once here.
constexpr u64 kParentIndexField = 1;

/// Where an `Entity`'s index sits inside it. `Entity`'s members are private, so this cannot be
/// taken with `offsetof` — and it is not guessed either: the assertion below constructs an entity
/// with a known index and reads the word back, so reordering the two halves of `Entity` fails this
/// file at compile time rather than producing a hash of the generation.
constexpr u32 kEntityIndexOffset = 0;

static_assert(sizeof(Entity) == sizeof(std::array<u32, 2>));
static_assert(std::bit_cast<std::array<u32, 2>>(Entity::make(7, 9))[kEntityIndexOffset / 4] == 7,
              "an Entity's index is no longer its first word; the Parent schema's offset is wrong "
              "and would hash the generation, which is recycling history");

}  // namespace

Status declare_relationship_state(determinism::StateSchema& schema, ComponentTypeId parent,
                                  ComponentTypeId children) noexcept {
    if (parent == kInvalidComponent || children == kInvalidComponent) {
        return fail(ErrorCode::InvalidArgument,
                    "the relationship components are not registered in this world; a schema over "
                    "an invalid component id would address nothing");
    }

    const StateField parent_fields[] = {
        StateField{"parent.index", kParentIndexField,
                   static_cast<u32>(offsetof(Parent, value)) + kEntityIndexOffset,
                   reflect::FieldKind::U32, SimulationClass::Authoritative,
                   determinism::StateEncoding::Direct},
    };
    if (Status declared = schema.declare(SchemaSubject{parent}, kParentComponentName,
                                         Span<const StateField>(parent_fields, 1));
        !declared) {
        return declared;
    }

    // No fields. See the header: the buffer's order is unspecified, so hashing it would hash
    // operation history, and the edge is covered by `Parent` from the other side.
    return schema.declare(SchemaSubject{children}, kChildrenComponentName,
                          Span<const StateField>());
}

}  // namespace cy::ecs
