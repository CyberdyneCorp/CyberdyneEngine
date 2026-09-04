#pragma once
// The three authoring asset kinds, and the identifiers everything in this module addresses by.
// Tasks 3.2.1 and 3.2.4.
//
// `serialization-and-prefabs` opens by refusing to collapse prefab, scene and world into one
// container: "Prefabs and scenes MAY share a serialised representation, but the distinction SHALL
// be preserved in the asset model, because the editor workflows, the cooking behaviour, and the
// runtime meaning differ." So there is one `Document` (document.h) and an `AssetKind` on it, and
// every operation that differs between the kinds checks the kind rather than the file extension.
//
//   Prefab   one reusable thing, instantiable many times.       A robot, a door, a turret.
//   Scene    a reusable spatial composition, in local space.    A house interior, a village.
//   World    the top-level universe, composed of scenes.        A planet, a campaign map.
//
// --- LOCAL IDS ARE THE WHOLE REFERENCE MODEL
// ------------------------------------------------------
//
// "References between entities within a scene SHALL be serialized as stable local ids assigned at
// author time and preserved across saves, not as array indices or runtime entity ids."
//
// A `LocalId` is therefore a number a document assigns once and never reuses, and every reference —
// a parent link, a field holding an entity, an override's target — is one. Three consequences worth
// stating, because each of them is a scenario in the specification:
//
//   * reordering the entities in a file changes nothing, because nothing addresses a position;
//   * moving an entity between authoring chunk files changes nothing, because identity does not
//     live in the file it happens to be written in;
//   * an entity a prefab instance contributes gets a local id **in the containing document** at
//     placement time (see `PrefabInstance` in document.h), so a reference from one instance into
//     another is an ordinary local id and not a path.
//
// That last one is a decision rather than an obligation. The alternative is to address an entity
// inside an instance by a path of (instance, prefab-local id) pairs, which is what an override has
// to do — an override is authored against the prefab's own identifiers, and the prefab may be
// re-laid-out. A *reference*, though, points at one concrete entity in one concrete document, and
// giving it a document local id makes it survive exactly the refactor that an override survives by
// being addressed differently.

#include <cy/core/base/types.h>
#include <cy/core/values/asset_id.h>

#include <compare>

namespace cy::scene::serialization {

/// Which of the three kinds a document is.
enum class AssetKind : u8 {
    Prefab = 0,
    Scene = 1,
    World = 2,
};

const char* asset_kind_name(AssetKind kind) noexcept;

/// An entity's stable identity within one authoring document. Assigned once, never reused.
///
/// A distinct type rather than a bare `u32` because this module deals in three different numbers
/// that are all counters — a local id, a parameter id, and a resolved index — and the compiler
/// catching a mix-up costs nothing here and everything at three in the morning.
class LocalId {
public:
    constexpr LocalId() noexcept = default;
    explicit constexpr LocalId(u32 value) noexcept : value_(value) {}

    [[nodiscard]] constexpr u32 value() const noexcept { return value_; }
    /// Zero is "no entity", so a default-constructed reference is absent rather than pointing at
    /// whichever entity happened to be written first.
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    friend constexpr bool operator==(LocalId, LocalId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(LocalId, LocalId) noexcept = default;

private:
    u32 value_ = 0;
};

inline constexpr LocalId kNoLocalId{};

/// The identity of one exposed parameter within its prefab. Resolved from a name once, at authoring
/// time, so that "runtime parameter application SHALL NOT require string lookup" is structural: an
/// argument carries this and no name at all.
class ParameterId {
public:
    constexpr ParameterId() noexcept = default;
    explicit constexpr ParameterId(u32 value) noexcept : value_(value) {}

    [[nodiscard]] constexpr u32 value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    friend constexpr bool operator==(ParameterId, ParameterId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(ParameterId, ParameterId) noexcept = default;

private:
    u32 value_ = 0;
};

/// What a scene instance becomes when the world it sits in is cooked.
///
/// `Embedded` is the default for static environment content "because it costs nothing at runtime";
/// `Packed` is for a composition that must move or be owned as one — a ship interior, a train, an
/// elevator.
enum class CookMode : u8 {
    Embedded = 0,
    Packed = 1,
};

const char* cook_mode_name(CookMode mode) noexcept;

/// Whether an entity moves at runtime. The input to the flattening decision, and authored rather
/// than inferred: the cooker cannot know that a turret's barrel will be rotated by a system that
/// does not exist yet.
enum class MotionKind : u8 {
    /// Never moves relative to its parent after the world is built.
    Static = 0,
    /// Animates, detaches, is driven by a system, or is queried for its relationship.
    Dynamic = 1,
};

/// The per-entity override of the flattening decision. "The decision SHALL be overridable per
/// entity."
enum class FlattenPolicy : u8 {
    /// Decide from motion, walking to the root. The default and the right answer almost always.
    Automatic = 0,
    /// Keep the relationship whatever the motion analysis concludes.
    Keep = 1,
    /// Remove the relationship and bake the transform, whatever the motion analysis concludes.
    /// A deliberate act: it is how a designer says "this thing is welded on" about something the
    /// analysis has no way to know is welded on.
    Flatten = 2,
};

const char* flatten_policy_name(FlattenPolicy policy) noexcept;

/// The recommended maximum depth of a variant chain.
///
/// "Variant chains SHALL have a configurable depth limit with a recommended maximum, and exceeding
/// the recommendation SHALL warn: beyond a few levels, reasoning about where a value comes from
/// becomes impractical, and composition is the better tool." Three is the recommendation; the limit
/// is configurable per library (library.h) and this is what it defaults to.
inline constexpr u32 kRecommendedVariantDepth = 3;

}  // namespace cy::scene::serialization
