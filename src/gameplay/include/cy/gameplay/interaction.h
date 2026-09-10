#pragma once
// The interaction framework. M8.b task 3.3.
//
// `gameplay-framework` — "Interaction": "an interactor queries what is available, an interactable
// provides **options**, and selecting one produces a gameplay command." An option carries "its
// action tag, display text, range, conditions, and the command it produces — so the interface can
// present it and artificial intelligence can evaluate it without either knowing the
// implementation." Queries are **batchable** and use spatial acceleration; "per-entity ray casts
// from thousands of agents SHALL NOT be the mechanism". Precision is selectable.
//
// ================================================================================================
// WHY SELECTING AN OPTION RETURNS A COMMAND RATHER THAN DOING SOMETHING
// ================================================================================================
//
// Because the alternative is a second door into the simulation. `command.h`'s whole argument is
// that intent reaches gameplay only as commands, so an interaction that applied its own effect
// would be a system reading a side channel — exactly the shape `tests/test_bypass.cpp` shows
// diverging in replay. `select()` therefore builds a `Command` and hands it back; the caller
// records it in its producer's buffer like any other.
//
// ================================================================================================
// THE GRID IS THE REQUIREMENT, NOT AN OPTIMISATION
// ================================================================================================
//
// "WHEN thousands of agents evaluate nearby interactions THEN queries SHALL be batched against
// spatial structures." A uniform grid over the interactables, one cell sweep per query, and a
// report of how many cells and candidates were touched — so a test can assert that a query over a
// thousand interactables examined a handful rather than a thousand. Without that number the
// requirement is unmeasurable and the first refactor that turns it back into a scan is invisible.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>
#include <cy/core/values/name.h>
#include <cy/ecs/entity.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/tags.h>

namespace cy::gameplay {

/// How exact a query is. "A focused human interaction may be precise while bulk agent queries use
/// coarser spatial tests."
enum class InteractionPrecision : u8 {
    /// The cell sweep alone: everything in range's cells is a candidate.
    Coarse = 0,
    /// The cell sweep, then an exact distance test against the option's range.
    Precise,
    Count,
};

/// One thing an interactable offers. Data — the interface presents it and an agent evaluates it,
/// and neither knows what implements it.
struct InteractionOption {
    /// What the option IS: `Interact.Mine`, `Interact.Repair`. A gameplay tag, so an agent can
    /// reason about kinds it has never heard of.
    TagId action = kInvalidTag;
    /// What a player reads. Presentation only.
    Name display_text;
    f32 range = 0.0F;
    /// Conditions, as tags the interactor must and must not carry.
    TagId required_tag = kInvalidTag;
    TagId forbidden_tag = kInvalidTag;
    /// The command selecting it produces.
    CommandTypeId command = kInvalidCommandType;
};

/// One option found by a query, and the interactable that offers it.
struct InteractionCandidate {
    ecs::Entity interactable;
    u32 option = 0;
    f32 distance = 0.0F;
};

/// What one interactor asked.
struct InteractionQuery {
    ecs::Entity interactor;
    Vec3 origin;
    f32 radius = 0.0F;
    InteractionPrecision precision = InteractionPrecision::Precise;
    /// Where this query's results begin in the batch's result array. Filled by `query_batch`.
    u32 first_result = 0;
    u32 result_count = 0;
};

/// What a batch cost. The measurement that keeps "batched against spatial structures" honest.
struct InteractionQueryReport {
    u32 queries = 0;
    u32 cells_visited = 0;
    /// Options actually distance-tested. Compare against `interactable_count()` to see whether the
    /// grid did anything.
    u32 candidates_tested = 0;
    u32 results = 0;
};

/// The interactables, their options, and the batched spatial query over them.
class InteractionRegistry {
public:
    /// The grid's cell edge, in world units. Interaction ranges are metres, so metres is the scale
    /// a cell should be; a project with a different scale passes its own.
    InteractionRegistry(Allocator& allocator, f32 cell_size = 4.0F) noexcept;

    InteractionRegistry(const InteractionRegistry&) = delete;
    InteractionRegistry& operator=(const InteractionRegistry&) = delete;

    [[nodiscard]] Status add_interactable(ecs::Entity entity, const Vec3& position) noexcept;
    [[nodiscard]] Status move_interactable(ecs::Entity entity, const Vec3& position) noexcept;
    void remove_interactable(ecs::Entity entity) noexcept;
    [[nodiscard]] Status add_option(ecs::Entity entity, const InteractionOption& option) noexcept;

    [[nodiscard]] u32 interactable_count() const noexcept {
        return static_cast<u32>(interactables_.size());
    }
    [[nodiscard]] u32 option_count() const noexcept { return static_cast<u32>(options_.size()); }
    [[nodiscard]] const InteractionOption& option_at(u32 index) const noexcept {
        return options_[index].option;
    }

    /// Answer many interactors at once. `queries` is updated in place with each query's slice of
    /// `results`, so a caller reads one array rather than one per agent.
    [[nodiscard]] Status query_batch(Span<InteractionQuery> queries, const EntityTagStore& tags,
                                     const TagRegistry& registry,
                                     Array<InteractionCandidate>& results,
                                     InteractionQueryReport& report) const noexcept;

    /// Selecting an option produces a command. It does not perform anything — see the header
    /// comment.
    [[nodiscard]] Expected<Command, Error> select(const InteractionCandidate& candidate,
                                                  ParticipantId participant,
                                                  ControlSourceId source) const noexcept;

private:
    struct Interactable {
        ecs::Entity entity;
        Vec3 position;
        u32 first_option = 0;
        u32 option_count = 0;
        /// The cell it is filed under, so moving it removes it from one cell rather than from a
        /// walk over all of them.
        u64 cell = 0;
    };
    struct OptionRow {
        ecs::Entity entity;
        InteractionOption option;
    };
    struct Cell {
        u64 key = 0;
        Array<u32> members;
    };

    [[nodiscard]] u32 find_interactable(ecs::Entity entity) const noexcept;
    [[nodiscard]] Cell* find_cell(u64 key) noexcept;
    [[nodiscard]] const Cell* find_cell(u64 key) const noexcept;
    [[nodiscard]] Status insert_into_cell(u32 index, const Vec3& position) noexcept;
    void remove_from_cell(u32 index) noexcept;
    [[nodiscard]] i32 cell_of(f32 coordinate) const noexcept;
    /// Three cell coordinates packed into one key: twenty-one bits each, which covers a world two
    /// million cells across in every direction.
    [[nodiscard]] static u64 cell_key(i32 x, i32 y, i32 z) noexcept;

    Allocator* allocator_;
    Array<Interactable> interactables_;
    /// Entity -> its row. A scan here would make building a thousand interactables quadratic,
    /// which is the shape this whole file exists to avoid.
    HashMap<u64, u32> by_entity_;
    Array<OptionRow> options_;
    Array<Cell> cells_;
    /// Cell key -> its index in `cells_`. The grid is an index; finding a cell by walking every
    /// cell would make it a list with extra steps.
    HashMap<u64, u32> by_cell_;
    f32 cell_size_;
};

}  // namespace cy::gameplay
