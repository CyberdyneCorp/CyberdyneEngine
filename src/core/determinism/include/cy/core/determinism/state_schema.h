#pragma once
// What participates in the hash, and it is DECLARED rather than discovered. Task 4.2.5 / 4.2.6.
//
// design.md §5's seed list is explicit: "state that participates in the hash is declared rather
// than discovered". This file is that declaration. Nothing is hashed because it happened to be
// reachable; a subject — a component type, a subsystem's block of state — is hashed exactly when
// something declared a schema for it, and only the fields that schema names.
//
// --- WHY THIS EXISTS BESIDE REFLECTION -----------------------------------------------------------
//
// Two reasons, and both are load-bearing rather than temporary:
//
//   1. M1's attribute set has `PersistenceKind` and no simulation class. `Predicted` and
//      `Presentation` — the two classes the determinism firewall is actually about — have no
//      enumerator to be derived from, so a field that is one of them has to say so somewhere.
//      `declare_reflected()` derives what it can and this file's explicit form declares the rest.
//      Adding a `SimulationClass` attribute to the generator would collapse the two into one, and
//      that is a change to `src/core/reflect/`.
//   2. Not every component is reflected. The ECS's `Parent`/`Children` and every one of the scene's
//      twelve built-ins are registered by name with no `TypeInfo` behind them (see those modules'
//      READMEs). Discovery would silently hash none of them and report a healthy-looking number; a
//      declaration makes their absence a fact the report states.
//
// --- WHAT AN UNDECLARED SUBJECT MEANS ------------------------------------------------------------
//
// It is not hashed, and the walk *counts it*. `simulation-and-determinism` forbids hashing raw
// structure memory as canonical authoritative state, so "hash its bytes because we do not know
// better" is not available; the honest alternative is to say how much state the hash does not
// cover, which is what `WorldHashReport::subjects_undeclared` is for. A hash that silently covers a
// tenth of the world is worse than one that says it does.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/classification.h>
#include <cy/core/determinism/hash.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/type_info.h>

namespace cy::determinism {

/// What a schema is about, as an opaque number.
///
/// Layer 0 cannot name `ecs::ComponentTypeId`, and this file deliberately does not want to: a
/// subsystem's state provider is a subject too, and so is a rules block. The runtime binds a
/// component's id into one of these and says so at the binding (`src/runtime/state_hash.h`).
struct SchemaSubject {
    u32 value = 0;

    friend constexpr bool operator==(SchemaSubject, SchemaSubject) noexcept = default;
};

/// One field of one subject, resolved to an offset and a width.
///
/// This is a *value* description, not a memory description: `kind` says how to read the bytes, so
/// the hash mixes a number and never a padding byte. A kind the hasher cannot read is refused at
/// declaration rather than skipped at hash time.
struct StateField {
    const char* name = "";
    /// The identity the divergence report names. From the reflected `FieldId` where there is one,
    /// and chosen by the declarer otherwise — it must be stable across runs and unique within the
    /// subject, and both are checked.
    u64 id = 0;
    u32 offset = 0;
    reflect::FieldKind kind = reflect::FieldKind::Unsupported;
    SimulationClass classification = SimulationClass::Authoritative;
};

/// Every field of one subject, in the order the hash folds them.
struct SubjectSchema {
    SchemaSubject subject;
    const char* name = "";
    /// Indices into the schema's field array. Contiguous, so a subject's fields are one span.
    u32 first_field = 0;
    u32 field_count = 0;
    /// How many of `field_count` are hashed. Zero means the subject is declared and contributes
    /// nothing, which is a different fact from "not declared" and is reported separately.
    u32 hashed_field_count = 0;
};

/// The declarations, and the one place a consumer asks what a subject contains.
class StateSchema {
public:
    explicit StateSchema(Allocator& allocator) noexcept
        : subjects_(allocator), fields_(allocator) {}

    StateSchema(const StateSchema&) = delete;
    StateSchema& operator=(const StateSchema&) = delete;

    /// Declare a subject explicitly. The route a built-in component takes.
    ///
    /// Refuses: a duplicate subject, a duplicate field id within the subject, a field whose kind
    /// the hasher cannot read, and a declaration made after `freeze()`. Each of those is a defect
    /// that would otherwise show up as a hash that quietly means something else.
    [[nodiscard]] Status declare(SchemaSubject subject, const char* name,
                                 Span<const StateField> fields) noexcept;

    /// Declare a subject from a reflected type, deriving each field's class from its
    /// `PersistenceKind` (classification.h's `class_of`). `Transient` fields are excluded, for the
    /// same reason serialization excludes them: they are declared not to be state.
    ///
    /// A field whose class needs to be `Predicted` or `Presentation` cannot be expressed this way;
    /// use `declare()` and say so. `override_classification` exists for exactly that case without
    /// forcing the caller to restate every other field.
    [[nodiscard]] Status declare_reflected(SchemaSubject subject,
                                           const reflect::TypeInfo& type) noexcept;

    /// Change one already-declared field's class. Refused after `freeze()`, and refused for a field
    /// the subject does not have — a typo that silently did nothing would be the worst outcome
    /// here, because the field would keep its derived class and the hash would keep covering it.
    [[nodiscard]] Status override_classification(SchemaSubject subject, u64 field_id,
                                                 SimulationClass classification) noexcept;

    /// Finalise. Sorts the subjects by their number, which is a stable identifier, so that a walk
    /// over the schema is in the same order whatever order the declarations arrived in —
    /// `simulation-and-determinism`'s "Registration and initialisation order".
    void freeze() noexcept;
    [[nodiscard]] bool frozen() const noexcept { return frozen_; }

    [[nodiscard]] const SubjectSchema* find(SchemaSubject subject) const noexcept;
    [[nodiscard]] Span<const StateField> fields_of(const SubjectSchema& subject) const noexcept;
    [[nodiscard]] u32 subject_count() const noexcept { return static_cast<u32>(subjects_.size()); }
    [[nodiscard]] const SubjectSchema& subject_at(u32 index) const noexcept {
        return subjects_[index];
    }

private:
    [[nodiscard]] SubjectSchema* find_mutable(SchemaSubject subject) noexcept;

    Array<SubjectSchema> subjects_;
    Array<StateField> fields_;
    bool frozen_ = false;
};

/// True when the hasher can read a field of this kind by value. Every M1 kind but `Unsupported` is
/// readable; the predicate exists so that the first kind that is not fails at declaration rather
/// than being skipped at hash time.
[[nodiscard]] bool kind_is_hashable(reflect::FieldKind kind) noexcept;

/// Fold one field's value into the open node of `tree`. `base` is the address of the subject's
/// value — a component's row in a chunk, a provider's struct.
///
/// Reads by kind and never by size, which is what keeps padding out of the hash.
void hash_field(StateHashTree& tree, const StateField& field, const void* base) noexcept;

}  // namespace cy::determinism
