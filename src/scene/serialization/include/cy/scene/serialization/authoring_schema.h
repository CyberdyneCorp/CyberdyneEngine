#pragma once
// The authoring schema: the engine's registered component types, as an editor must name them.
// M6 task 2.4.
//
// --- WHAT PROBLEM THIS SOLVES
// ---------------------------------------------------------------------
//
// M5.5's gate: "`DocumentService::open` calls `Document::new` — a name and an EMPTY SCHEMA —
// because there is no world loader." The editor is a separate process in a different language, and
// it cannot see `cy::reflect::TypeRegistry`. Until it can, nothing in a document is described:
// nothing is selectable, no `Transform` binds, and a gizmo drag commits nothing.
//
// This is the half of the answer the ENGINE owns. `build_authoring_schema` reads the type registry
// and `write_authoring_schema` writes a deterministic text manifest that the editor reads
// (`cy_editor_services::worldfile`). The engine remains the source of truth for what a component is
// called and what it holds; the editor holds no second list.
//
// --- THE ONE TRANSFORMATION THIS PERFORMS, AND WHY IT IS NOT COSMETIC
// -----------------------------
//
// Reflection describes `cy::scene::LocalTransform` as TEN `f32` fields named `value.rotation.x`,
// `value.rotation.y`, ... `value.scale.z`. That is the right description for a serializer copying
// bytes and the wrong one for a person: an inspector generated from it shows ten spin boxes, and a
// gizmo has nothing to bind to, because a gizmo moves a position and turns a rotation rather than
// editing `value.translation.y`.
//
// So consecutive scalar lanes with a common dotted prefix are **grouped**: `p.x, p.y, p.z` becomes
// one `Vec3` named `p`, and `p.x, p.y, p.z, p.w` becomes one `Quat`. The group takes the identifier
// of its FIRST lane, which is what keeps the grouping addressable: an override, a history entry or
// a migration naming that identifier still names the same bytes.
//
// The rule is applied to every registered type rather than to a list of known ones, so a plugin's
// own transform-like component reaches the editor as a transform without anybody adding it here.
//
// --- AND THE ONE ALIAS, WHICH IS DECLARED RATHER THAN DERIVED
// -------------------------------------
//
// `editor-viewport-and-gizmos` binds a gizmo to a component named `Transform` with fields
// `translation`, `rotation` and `scale`. Grouping gives the three field names for free — the
// reflected names really are `value.translation`, `value.rotation` and `value.scale`. The TYPE name
// does not follow from anything: the engine's component is `cy::scene::LocalTransform`, because in
// the ECS it is the local half of a transform pair, and the editor's authoring concept is
// `Transform`, because that is what a designer moves.
//
// One declared alias is therefore the honest spelling. Deriving it — stripping a `Local` prefix,
// say — would be a rule that silently renames the next component somebody adds.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/registry.h>

#include <string_view>

namespace cy::scene::serialization {

/// What an authoring field holds, in the editor's value vocabulary.
///
/// These names are written into the manifest and parsed by `cy_editor_core::value::ValueKind`, so
/// they are a contract with the editor and not a local spelling. `Nil` exists because the editor's
/// enumeration has it; nothing here produces one.
enum class AuthoringKind : u8 {
    Bool,
    Int,
    Float,
    Double,
    Vec2,
    Vec3,
    Vec4,
    Quat,
    Text,
    Bytes,
    Entity,
};

/// The enumerator's spelling, which is what the manifest carries. Never null.
[[nodiscard]] const char* authoring_kind_name(AuthoringKind kind) noexcept;

/// One field an editor may show and edit.
struct AuthoringField {
    /// The identifier of the field, or of the FIRST lane of a grouped one.
    reflect::FieldId id;
    AuthoringKind kind = AuthoringKind::Float;
    /// A view into the reflected metadata, which is static for the life of the process.
    std::string_view name;
    std::string_view description;
};

/// One component type an editor may author.
struct AuthoringType {
    explicit AuthoringType(Allocator& allocator) noexcept : fields(allocator) {}

    AuthoringType(const AuthoringType&) = delete;
    AuthoringType& operator=(const AuthoringType&) = delete;
    AuthoringType(AuthoringType&&) noexcept = default;
    AuthoringType& operator=(AuthoringType&&) noexcept = default;
    ~AuthoringType() = default;

    reflect::TypeId id;
    /// Whether the type exists only while authoring and must not reach a runtime world.
    bool authoring_only = false;
    /// The name the editor uses, which is the alias when there is one.
    std::string_view name;
    Array<AuthoringField> fields;
};

/// Every component type an editor may author, in ascending identifier order.
class AuthoringSchema {
public:
    explicit AuthoringSchema(Allocator& allocator = current_allocator()) noexcept
        : types_(allocator), allocator_(&allocator) {}

    [[nodiscard]] Array<AuthoringType>& types() noexcept { return types_; }
    [[nodiscard]] const Array<AuthoringType>& types() const noexcept { return types_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }

private:
    Array<AuthoringType> types_;
    Allocator* allocator_;
};

/// The editor's name for a reflected type, or the type's own unqualified name.
///
/// Public so that the alias table is readable from a test rather than only observable through its
/// output — a table nobody can enumerate is a table that silently grows.
[[nodiscard]] std::string_view authoring_name_of(std::string_view reflected_name) noexcept;

/// Build the authoring schema of everything `registry` holds.
///
/// Ordered by `TypeId`, so two runs over the same registry produce the same schema and therefore
/// the same bytes. A registry's own iteration order is its hash table's, which is not that.
[[nodiscard]] Status build_authoring_schema(const reflect::TypeRegistry& registry,
                                            AuthoringSchema& out) noexcept;

/// Write the manifest the editor reads. Appends; `out` is not cleared.
///
/// The grammar, which `cy_editor_services::worldfile` implements the other half of:
///
///     cyschema 1
///     type <id> <authoring|runtime> "<name>"
///       field <id> <kind> "<name>" "<description>"
[[nodiscard]] Status write_authoring_schema(const AuthoringSchema& schema,
                                            Array<char>& out) noexcept;

/// The manifest's first word and version, restated here so a test can assert on them.
inline constexpr std::string_view kAuthoringSchemaMagic = "cyschema";
inline constexpr u32 kAuthoringSchemaVersion = 1;

}  // namespace cy::scene::serialization
