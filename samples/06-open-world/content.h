#pragma once
// samples/06-open-world — the content: what a shipped world file says, and the cells it cooks into.
//
// THE WORLD IS NOT COMMITTED, ITS DESCRIPTION IS. Six kilometres of city at 128 m cells is 2,304
// cells and 92,160 persistent entities; a repository carrying those would be measuring git. So
// `project/world/*.cyopenworld` holds numbers, `cy_build` cooks the two of them into one shipped
// stream, and `cook_cell()` below turns that stream into `cy::world::CookedCell`s — deterministic
// in the declared seed, so two runs, two machines and two profiles cook one world.
//
// EVERY CELL IS COOKED IN ECS-NATIVE FORM, through `cy::world::CellBuilder`. That is not an
// implementation detail of this sample: `world-partition-and-streaming` requires cooking to produce
// archetype blocks ready to be copied into ECS chunks rather than an object graph to be walked at
// load, and a sample that built entities one at a time would be demonstrating the thing the
// specification forbids.
//
// FOUR COMPONENTS, AND EACH IS HERE FOR A REASON:
//
//   Ident      the persistent identifier, carried on the row. A live row can then name itself,
//              which is what makes "the entity the save removed is absent after reload" a thing
//              this program can check rather than infer from an ordering.
//   Placement  where the prop is inside its cell. Cell-relative, per `world-partition-and-
//              streaming`'s coordinate rule; nothing here holds an absolute position.
//   Prop       kind and variant: what the traversal reports and what a content change moves.
//   Structure  material and integrity, the sample's PersistentState fields. These are what a save
//              carries, and the only fields `save::Overlay::record_component` writes — the
//              classification comes from the descriptors below and not from a call site.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/type_info.h>
#include <cy/ecs/world.h>
#include <cy/world/cell.h>
#include <cy/world/partition.h>

#include <string_view>

namespace cy::sample::openworld {

/// The persistent identifier, on the row. See the header comment.
struct Ident {
    u64 value = 0;
};

/// Where a prop sits inside its own cell.
struct Placement {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 z = 0.0F;
};

/// What a prop is. `kind` indexes the palette; a landmark carries the kind its content line names.
struct Prop {
    u32 kind = 0;
    u32 variant = 0;
};

/// The half of a prop a save carries.
struct Structure {
    u32 material = 0;
    u32 integrity = 0;
};

[[nodiscard]] const reflect::TypeInfo& ident_type() noexcept;
[[nodiscard]] const reflect::TypeInfo& placement_type() noexcept;
[[nodiscard]] const reflect::TypeInfo& prop_type() noexcept;
[[nodiscard]] const reflect::TypeInfo& structure_type() noexcept;

/// The component identifiers this world's cells are cooked against, in one world.
struct Components {
    ecs::ComponentTypeId ident = ecs::kInvalidComponent;
    ecs::ComponentTypeId placement = ecs::kInvalidComponent;
    ecs::ComponentTypeId prop = ecs::kInvalidComponent;
    ecs::ComponentTypeId structure = ecs::kInvalidComponent;
};

[[nodiscard]] Expected<Components, Error> register_components(ecs::World& world) noexcept;

/// One entry of the prop palette.
struct PropKind {
    u32 kind = 0;
    u32 weight = 1;
    u32 material = 0;
    char name[32] = {};
};

/// One landmark: a single further entity in a named cell, which the route reports passing.
struct Landmark {
    i32 x = 0;
    i32 z = 0;
    u32 kind = 0;
    char name[32] = {};
};

/// A parsed `cyopenworld 1` stream. Everything the cook needs and nothing else.
class WorldContent {
public:
    explicit WorldContent(Allocator& allocator) noexcept
        : palette_(allocator), landmarks_(allocator) {}

    /// Parse a shipped stream. `text` may carry SEVERAL `cyopenworld 1` headers, because the
    /// shipped stream is the concatenation of the sources the build graph cooked — see
    /// openworld.cybuild.
    ///
    /// Fails with `InvalidArgument` naming the line, so a content error is a diagnostic rather than
    /// a world that silently comes out empty.
    [[nodiscard]] Status parse(std::string_view text) noexcept;

    [[nodiscard]] i32 extent() const noexcept { return extent_; }
    [[nodiscard]] f32 cell_size() const noexcept { return cell_size_; }
    [[nodiscard]] u64 seed() const noexcept { return seed_; }
    [[nodiscard]] u32 props_per_cell() const noexcept { return props_per_cell_; }
    [[nodiscard]] const char* name() const noexcept { return name_; }
    [[nodiscard]] Span<const PropKind> palette() const noexcept { return palette_.span(); }
    [[nodiscard]] Span<const Landmark> landmarks() const noexcept { return landmarks_.span(); }

    /// Cells in the world, and entities in it. What the report prints.
    [[nodiscard]] u32 cell_count() const noexcept;
    [[nodiscard]] u64 entity_count() const noexcept;
    /// Metres per side.
    [[nodiscard]] f64 span_metres() const noexcept;

    /// A deterministic digest of the parsed content. Two builds of one world agree; a patched world
    /// does not, which is how the run says out loud that it is playing different content.
    [[nodiscard]] u64 digest() const noexcept;

    /// The landmark in a cell, or null.
    [[nodiscard]] const Landmark* landmark_at(i32 x, i32 z) const noexcept;

    [[nodiscard]] world::PartitionConfig partition() const noexcept;

private:
    [[nodiscard]] Status parse_line(std::string_view line, u32 number) noexcept;

    Array<PropKind> palette_;
    Array<Landmark> landmarks_;
    char name_[32] = {};
    i32 extent_ = 0;
    f32 cell_size_ = 128.0F;
    u64 seed_ = 0;
    u32 props_per_cell_ = 0;
};

/// Cook one cell of the described world. Deterministic in (content, coordinate) and in nothing
/// else.
[[nodiscard]] world::CookedCell cook_cell(Allocator& allocator, const WorldContent& content,
                                          const world::Partitioner& partitioner,
                                          world::CellCoord coord,
                                          const Components& components) noexcept;

/// The persistent identifier of row `row` of the cell at `(x, z)`. Assigned at cook time from the
/// cell's place in the world and never from a pointer, an index into a live array or a clock — the
/// same identifier in every run, which is the whole of "persistent identity survives a reload".
[[nodiscard]] world::PersistentId identity_of(const WorldContent& content, i32 x, i32 z,
                                              u32 row) noexcept;

}  // namespace cy::sample::openworld
