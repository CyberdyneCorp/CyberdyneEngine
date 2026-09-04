#pragma once
// Parent and child, as components the world maintains. Task 2.9.
//
// `ecs-core` — "Entity relationships": parent-child is a first-class relation held as `Parent` and
// `Children` components "kept consistent by the world rather than by user code". That sentence is
// the design: nothing outside src/ecs/ writes either component, and the world's reparent and
// destroy paths are the only places both sides are touched — which is why they can be updated
// together and why "atomically at the flush point" is a property of one function rather than a
// convention.
//
// THESE TWO ARE BUILT-IN COMPONENTS, NOT REFLECTED ONES (component.h). They carry entity references
// rather than authored data, never appear in a prefab, and are reconstructed rather than loaded, so
// they have no manifest identifier and a serialized world names them instead of numbering them.

#include <cy/ecs/entity.h>

namespace cy::ecs {

/// The entity's parent. A data component: one entity reference, whose offset is declared to the
/// registry so serialization remaps it without asking reflection what the field means.
struct Parent {
    Entity value;
};

/// The name `Parent` is registered under. A string rather than a number because a built-in has no
/// manifest identifier; see component.h.
inline constexpr const char* kParentComponentName = "cy::ecs::Parent";

/// The name `Children` is registered under. It is a buffer component of `Entity`.
inline constexpr const char* kChildrenComponentName = "cy::ecs::Children";

/// Children held inline before a buffer spills to the heap. Eight covers the overwhelming majority
/// of authored hierarchies — a node with a mesh, a collider and a few attachment points — and the
/// ones it does not are the ones a heap block is worth paying for.
inline constexpr u32 kInlineChildren = 8;

/// What happens to an entity's children when it is destroyed.
enum class DestroyPolicy : u8 {
    /// The subtree goes with it, in one deferred operation. `ecs-core`'s default.
    CascadeChildren = 0,
    /// The children are reparented to the destroyed entity's own parent. The opt-out.
    ReparentChildren = 1,
};

}  // namespace cy::ecs
