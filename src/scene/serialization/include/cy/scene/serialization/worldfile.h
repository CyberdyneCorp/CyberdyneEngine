#pragma once
// `.cyworld`, read and written by the ENGINE. M8.a tasks 1.1 and 1.2.
//
// ================================================================================================
// THE DEFECT THIS MODULE CLOSES, IN M7'S GATE'S OWN WORDS
// ================================================================================================
//
// *"`.cyworld` is a third authoring format that nothing under `src/` or `tools/` reads, beside
// `cydoc` and `CookedCell`"*, and *"the editor's document and the engine's scene are associated IN
// FIRST-SEEN ORDER by a file whose own header calls itself a stand-in"*.
//
// Those two sentences are one defect. The editor writes a world; the runtime renders a scene it
// built itself; and the only thing joining them is a counter that hands the editor's first-named
// identity the runtime's first object. Everything downstream of that — a created primitive, a body
// on it, a play session — is downstream of the two being one world.
//
// This module is the engine's half of making them one. `read_world` turns the bytes the editor
// saved into a `World` whose nodes carry **the editor's own stable identities**, and `write_world`
// writes the same bytes back. There is no second representation and no correspondence table: the
// identity in the runtime is the identity in the document, because both are computed from the same
// two numbers by the same function.
//
// ================================================================================================
// HOW AN IDENTITY CROSSES THE PROCESS BOUNDARY WITHOUT BEING SENT
// ================================================================================================
//
// `cy_editor_core::ids` derives every identity rather than allocating one:
//
//     DocumentId = FNV-1a-128(the document's asset path, as UTF-8)
//     NodeId     = FNV-1a-128(DocumentId as 16 little-endian bytes ++ ordinal as 8)
//
// and ordinals are issued from 1 in the order nodes are created. `cy_editor_services::worldfile`
// creates them in FILE ORDER, so the node written at position `p` of a `.cyworld` is the document's
// ordinal `p + 1` — every time, on both sides, with nothing transmitted.
//
// So this reader computes what the editor computed. `editor_document_identity` and
// `editor_node_identity` below are `fnv1a_128` restated in C++, and `test_worldfile.cpp` pins them
// against values produced independently, because a hash reimplemented and never compared is a hash
// that agrees until somebody changes a constant.
//
// **The protocol narrows to 64 bits** (`cy_editor_services::mirror::engine_identity` takes the low
// half of the `u128`, and `gizmo::Request::identities` is a `Vec<u64>`), so `WorldNode::identity`
// is that same low half and `WorldNode::full_identity` keeps the whole of it for a wire that one
// day carries it.
//
// ================================================================================================
// WHY THE FILE'S TYPE NUMBERS ARE NOT THE ENGINE'S, AND WHY THAT IS FINE
// ================================================================================================
//
// A `.cyworld` numbers its types and fields with the numbers the WRITING DOCUMENT'S schema issued —
// `DocumentSchema::declare_type` counts from one in declaration order — and those are not
// `reflect::TypeId`s. `samples/05b-editor-window/runtime/session.h` records what that cost: *"THE
// ONE THING IT CANNOT KNOW is which field that is"*, and the workaround was to treat a changed
// `Vec3` as a translation whenever the editor's last stated gizmo mode was Translate.
//
// It is fine because **the file carries the names**. Its `type` section spells out every type and
// field, which is what `serialization-and-prefabs` requires of tagged authoring data in the first
// place — *"an editor without a plugin does not silently strip that plugin's data"*. So
// `resolve_against` matches those names against the engine's own `AuthoringSchema` and fills in the
// `reflect::TypeId` and `reflect::FieldId` each one means here. A name this build has never heard
// of resolves to nothing and is CARRIED rather than dropped, so a round trip through a build
// missing a plugin still writes the plugin's data back out.
//
// ================================================================================================
// WHAT IS DELIBERATELY NOT HERE
// ================================================================================================
//
// A COOK. `cook.h` turns authoring documents into archetype blocks and this is not that: a world
// read here is the AUTHORING form, which is what the editor edits and what a live session mutates.
// Cooking it is a separate step with a separate output, and putting the two in one type is how a
// format ends up unable to represent a state the editor can reach.
//
// AN ECS WORLD. Nothing here creates entities. A caller that wants them spawns from this; a caller
// that only wants to draw the world reads `transform_of` and does not need an ECS at all, which is
// what lets `samples/05b-editor-window` hold the authored world without hosting a simulation.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/ids.h>
#include <cy/scene/serialization/authoring_schema.h>

#include <string_view>

namespace cy::scene::serialization {

/// The first word of a world file, and the version this build writes.
inline constexpr std::string_view kWorldMagic = "cyworld";
inline constexpr u32 kWorldVersion = 1;

/// A 128-bit editor identity, as two halves.
///
/// Two `u64`s rather than a compiler extension: `__uint128_t` is a GCC and Clang extension that
/// MSVC does not have, and an identity that existed on two of the three platforms would be an
/// identity the engine could not be built with.
struct EditorId {
    u64 low = 0;
    u64 high = 0;

    [[nodiscard]] friend constexpr bool operator==(const EditorId& left,
                                                   const EditorId& right) noexcept {
        return left.low == right.low && left.high == right.high;
    }
};

/// `DocumentId::of_asset` — FNV-1a-128 over the document's asset path.
///
/// The path is the one the EDITOR opened, spelled exactly as the editor spelled it: `DocumentId` is
/// a hash of that string and `worlds/city.cyworld` and `./worlds/city.cyworld` are two documents.
[[nodiscard]] EditorId editor_document_identity(std::string_view asset_path) noexcept;

/// `NodeId::in_document` — FNV-1a-128 over the document identity and the ordinal.
[[nodiscard]] EditorId editor_node_identity(const EditorId& document, u64 ordinal) noexcept;

/// What a field holds. The editor's `ValueKind`, which is what the manifest spells.
enum class WorldValueKind : u8 {
    Nil,
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

/// The spelling a manifest carries. Never null.
[[nodiscard]] const char* world_value_kind_name(WorldValueKind kind) noexcept;
/// The kind a manifest word names, or nothing when this build does not know the word.
[[nodiscard]] Expected<WorldValueKind, Error> world_value_kind_of(std::string_view name) noexcept;
/// The kind an authoring field of the engine's own schema presents as.
[[nodiscard]] WorldValueKind world_value_kind_of(AuthoringKind kind) noexcept;

/// One value, in the editor's vocabulary.
///
/// A flat record rather than a variant: it is written into an `Array` that a frame walks, it has to
/// be trivially copyable for that to cost nothing, and the widest member — four floats — is smaller
/// than the discriminated union's own alignment padding would be. `Text` and `Bytes` are a slice of
/// the world's blob pool, so a value never owns an allocation.
struct WorldValue {
    WorldValueKind kind = WorldValueKind::Nil;
    /// `Float` uses lane 0; `Vec2`, `Vec3`, `Vec4` and `Quat` use as many as they name.
    f32 lanes[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    /// `Int`, `Entity` and `Bool` (0 or 1).
    i64 integer = 0;
    /// `Double`.
    f64 real = 0.0;
    /// `Text` and `Bytes`: a slice of `World::blobs()`.
    u32 blob_offset = 0;
    u32 blob_length = 0;
};

/// A slice of a world's text pool.
struct WorldText {
    u32 offset = 0;
    u32 length = 0;
};

/// One field of one component on one node.
struct WorldField {
    /// The identifier the FILE gave it, which is the writing document's and not the engine's.
    u64 file_field = 0;
    /// What this build calls the same field, or an invalid id when the name is unknown here.
    reflect::FieldId engine_field;
    WorldValue value;
};

/// One component on one node.
class WorldComponent {
public:
    explicit WorldComponent(Allocator& allocator) noexcept : fields_(allocator) {}

    WorldComponent(const WorldComponent&) = delete;
    WorldComponent& operator=(const WorldComponent&) = delete;
    WorldComponent(WorldComponent&&) noexcept = default;
    WorldComponent& operator=(WorldComponent&&) noexcept = default;
    ~WorldComponent() = default;

    /// The identifier the file gave the type.
    u64 file_type = 0;
    /// What this build calls it, or an invalid id when the name is unknown here.
    reflect::TypeId engine_type;

    [[nodiscard]] Array<WorldField>& fields() noexcept { return fields_; }
    [[nodiscard]] const Array<WorldField>& fields() const noexcept { return fields_; }
    [[nodiscard]] WorldField* find(u64 file_field) noexcept;
    [[nodiscard]] const WorldField* find(u64 file_field) const noexcept;

private:
    Array<WorldField> fields_;
};

/// One node of the world: an object a person authored.
class WorldNode {
public:
    explicit WorldNode(Allocator& allocator) noexcept : components_(allocator) {}

    WorldNode(const WorldNode&) = delete;
    WorldNode& operator=(const WorldNode&) = delete;
    WorldNode(WorldNode&&) noexcept = default;
    WorldNode& operator=(WorldNode&&) noexcept = default;
    ~WorldNode() = default;

    /// The document ordinal this node was created with. One-based, as the editor's are.
    u64 ordinal = 0;
    /// The editor's whole 128-bit identity.
    EditorId full_identity;
    /// The low half, which is what the protocol carries and what a gizmo intent names.
    u64 identity = 0;
    /// The index of the parent node in `World::nodes()`, or `kNoParent` for a root.
    u32 parent = kNoParent;
    /// The authoring layer, a slice of `World::text()`.
    WorldText layer;
    /// Whether the node still exists. A delete does not compact the array, because every other
    /// node's index is a parent reference and compaction would rewrite them all.
    bool live = true;

    [[nodiscard]] Array<WorldComponent>& components() noexcept { return components_; }
    [[nodiscard]] const Array<WorldComponent>& components() const noexcept { return components_; }
    [[nodiscard]] WorldComponent* find(u64 file_type) noexcept;
    [[nodiscard]] const WorldComponent* find(u64 file_type) const noexcept;

    static constexpr u32 kNoParent = 0xFFFF'FFFFU;

private:
    Array<WorldComponent> components_;
};

/// One field of one declared type, as the file declares it.
struct WorldFieldDecl {
    u64 file_field = 0;
    WorldValueKind kind = WorldValueKind::Float;
    WorldText name;
    WorldText description;
    reflect::FieldId engine_field;
};

/// One declared type.
class WorldTypeDecl {
public:
    explicit WorldTypeDecl(Allocator& allocator) noexcept : fields_(allocator) {}

    WorldTypeDecl(const WorldTypeDecl&) = delete;
    WorldTypeDecl& operator=(const WorldTypeDecl&) = delete;
    WorldTypeDecl(WorldTypeDecl&&) noexcept = default;
    WorldTypeDecl& operator=(WorldTypeDecl&&) noexcept = default;
    ~WorldTypeDecl() = default;

    u64 file_type = 0;
    bool authoring_only = false;
    WorldText name;
    reflect::TypeId engine_type;

    [[nodiscard]] Array<WorldFieldDecl>& fields() noexcept { return fields_; }
    [[nodiscard]] const Array<WorldFieldDecl>& fields() const noexcept { return fields_; }
    [[nodiscard]] const WorldFieldDecl* find(u64 file_field) const noexcept;

private:
    Array<WorldFieldDecl> fields_;
};

/// One world: the schema it was written against, and its nodes.
///
/// **This is the authoritative representation, not a copy of one.** A runtime holding it and an
/// editor holding its document are looking at one world in two processes, and the transactions the
/// editor commits are applied to this by `apply_transaction`.
class World {
public:
    explicit World(Allocator& allocator = current_allocator()) noexcept
        : types_(allocator),
          nodes_(allocator),
          text_(allocator),
          blobs_(allocator),
          path_(allocator),
          allocator_(&allocator) {}

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) noexcept = default;
    World& operator=(World&&) noexcept = default;
    ~World() = default;

    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }

    [[nodiscard]] Array<WorldTypeDecl>& types() noexcept { return types_; }
    [[nodiscard]] const Array<WorldTypeDecl>& types() const noexcept { return types_; }
    [[nodiscard]] Array<WorldNode>& nodes() noexcept { return nodes_; }
    [[nodiscard]] const Array<WorldNode>& nodes() const noexcept { return nodes_; }

    /// The document's asset path, as the editor spelled it.
    [[nodiscard]] std::string_view path() const noexcept { return {path_.data(), path_.size()}; }
    [[nodiscard]] const EditorId& document() const noexcept { return document_; }
    /// The next ordinal the editor would issue. Kept in step so that a node created here has the
    /// identity the editor would have given it.
    [[nodiscard]] u64 next_ordinal() const noexcept { return next_ordinal_; }

    [[nodiscard]] Status set_path(std::string_view asset_path) noexcept;

    /// A view of a text slice.
    [[nodiscard]] std::string_view text(const WorldText& slice) const noexcept;
    /// A view of a blob slice.
    [[nodiscard]] Span<const u8> blob(const WorldValue& value) const noexcept;

    /// Intern text into the pool. Appends; slices are never freed, because a world is read, edited
    /// and written within one session and a pool that could shrink would invalidate every slice.
    [[nodiscard]] Expected<WorldText, Error> intern(std::string_view value) noexcept;
    /// The same for bytes, writing the slice into `value`.
    [[nodiscard]] Status intern_blob(Span<const u8> bytes, WorldValue& value) noexcept;

    /// The declared type a file identifier names.
    [[nodiscard]] const WorldTypeDecl* type(u64 file_type) const noexcept;
    /// The declared type an engine identifier names, once `resolve_against` has run.
    [[nodiscard]] const WorldTypeDecl* type_of(reflect::TypeId engine_type) const noexcept;
    /// The index of the node an editor identity names, or `WorldNode::kNoParent`.
    ///
    /// Linear. A world reaches thousands of nodes and this is called once per selected object per
    /// frame, so an index would be a map to keep in step for no measured gain; when a profile says
    /// otherwise the map goes here and nothing else changes.
    [[nodiscard]] u32 index_of(u64 identity) const noexcept;

    /// Append a node with the next ordinal, as the editor would. Returns its index.
    [[nodiscard]] Expected<u32, Error> create_node(u32 parent) noexcept;
    /// Append a node with a GIVEN ordinal, which is what applying an editor transaction does: the
    /// editor allocated the ordinal, and re-deriving one here would invent a second identity.
    [[nodiscard]] Expected<u32, Error> create_node_with_ordinal(u64 ordinal, u32 parent) noexcept;

    /// Adopt a document identity for `path`, recomputing every node's.
    void reidentify() noexcept;

    void set_next_ordinal(u64 ordinal) noexcept { next_ordinal_ = ordinal; }

private:
    Array<WorldTypeDecl> types_;
    Array<WorldNode> nodes_;
    Array<char> text_;
    Array<u8> blobs_;
    Array<char> path_;
    EditorId document_;
    u64 next_ordinal_ = 1;
    Allocator* allocator_;
};

/// What a read produced, so a caller can report it rather than only that it worked.
struct WorldReadReport {
    u32 types = 0;
    u32 fields = 0;
    u32 nodes = 0;
    u32 components = 0;
};

/// Read a `.cyworld`.
///
/// `asset_path` is how the EDITOR names the document — `worlds/city.cyworld`, not an absolute path
/// on disk — because that string is what both sides hash into a `DocumentId`. Getting it wrong is
/// not a load failure; it is a world whose identities nothing in the editor recognises, which is
/// why `verify_document_identity` exists and why the runtime calls it on the first transaction.
[[nodiscard]] Expected<WorldReadReport, Error> read_world(std::string_view text,
                                                          std::string_view asset_path, World& out,
                                                          WorldReadReport* report = nullptr);

/// Write a `.cyworld`. Appends to `out`; the inverse of `read_world`.
///
/// **Byte-identical on a round trip**, which `test_worldfile.cpp` asserts against the file the
/// editor wrote: floats are the shortest text that parses back to the same bits, lists are written
/// in the order they were read, and nothing here writes a timestamp, a path or a pointer.
[[nodiscard]] Status write_world(const World& world, Array<char>& out) noexcept;

/// Match the file's declared names against the engine's own schema.
///
/// Fills `engine_type` and `engine_field` where the names are known here and leaves them invalid
/// where they are not. Returns how many types resolved.
[[nodiscard]] Expected<u32, Error> resolve_against(World& world,
                                                   const AuthoringSchema& schema) noexcept;

/// Fill one component's engine identifiers from the world's already-resolved declarations.
///
/// What a component created AFTER `resolve_against` needs — one the editor added during the
/// session. Without it a freshly added `Transform` would be a component the engine could not
/// recognise as its own, which is the same defect as never resolving at all and much harder to see:
/// the world would be right and only the new object would be invisible.
void resolve_component(const World& world, WorldComponent& component) noexcept;

/// Whether a document identity carried in a transaction is this world's.
///
/// The runtime derives the identity from the path it was told to load; the editor derives it from
/// the path it opened. Comparing them on the first transaction turns a mismatched spelling into a
/// diagnostic instead of a world in which nothing the editor names exists.
[[nodiscard]] bool verify_document_identity(const World& world, const EditorId& claimed) noexcept;

/// The placement a node's `Transform` component describes, in the engine's own type.
///
/// Answers false when the node has no component the engine resolved to `cy::scene::LocalTransform`.
/// The three fields are found by their ENGINE identifiers, so this does not care what the file
/// numbered them and does not guess from a value's shape — which is exactly what
/// `samples/05b-editor-window/runtime/session.h` had to do and said it should not have to.
[[nodiscard]] bool transform_of(const World& world, const WorldNode& node, Transform& out) noexcept;

/// Write a placement back into a node's `Transform`. The inverse of `transform_of`.
[[nodiscard]] bool set_transform(World& world, WorldNode& node,
                                 const Transform& placement) noexcept;

}  // namespace cy::scene::serialization
