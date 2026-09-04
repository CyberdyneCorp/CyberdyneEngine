#pragma once
// Queries: matching, iteration and filtering. Task 2.4.
//
// `ecs-core` — "Queries": a query selects entities by `With<T...>`, `Without<T...>` and
// `Optional<T>`, with `Read<T>`/`Write<T>` access declarations; matching is **cached** and updated
// incrementally as archetypes appear; and change filtering and shared-component filtering are
// supported.
//
// THE CONSTRAINTS ARE RUNTIME VALUES, NOT TEMPLATE PARAMETERS, AND THAT IS DELIBERATE. A component
// id is per world (component.h), so a type-level `With<Transform>` would have to resolve to a
// different number in the editor world and the play-mode world. The type-level spelling is the
// convenience — `desc.write<Transform>(world)` — and it resolves to the same runtime term, so
// there is one matcher rather than one per instantiation.
//
// A QUERY IS ALSO WHERE A SYSTEM'S ACCESS DECLARATION COMES FROM. Every term records itself into a
// `jobs::AccessSet` as it is added: `With` is a Read, `write()` is a Write, `Without` is an
// Exclude. A system therefore cannot declare an access that disagrees with the query it runs,
// because it does not declare one twice — see system.h and task 2.5.
//
// CHANGE FILTERING IS CHUNK-GRANULAR AND SAYS SO. `ecs-core`: "WHEN one entity in a chunk is
// written THEN the whole chunk SHALL be considered changed; this is documented as chunk-granular,
// not entity-granular." A filtered iteration therefore skips whole chunks and never individual
// rows.

#include <cy/core/base/expected.h>
#include <cy/core/jobs/access.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/archetype.h>
#include <cy/ecs/world.h>

namespace cy::ecs {

/// What a query selects, and the access it therefore declares.
class QueryDesc {
public:
    explicit QueryDesc(Allocator& allocator) noexcept
        : required_terms_(allocator), optional_terms_(allocator) {}

    /// Required and read. The ordinary term.
    [[nodiscard]] Status read(ComponentTypeId component) noexcept;
    /// Required and written. Iterating it through `QueryChunk::write` stamps the chunk's version.
    [[nodiscard]] Status write(ComponentTypeId component) noexcept;
    /// Required, but not accessed — a presence constraint that reads no data.
    [[nodiscard]] Status with(ComponentTypeId component) noexcept;
    /// Excluded. Reads no component data, so it conflicts with nothing and never serialises a
    /// system against one that writes it — see `<cy/core/jobs/access.h>`.
    [[nodiscard]] Status without(ComponentTypeId component) noexcept;
    /// Accessed if present. Does not constrain matching; `QueryChunk::has` is what a body tests.
    [[nodiscard]] Status optional(ComponentTypeId component) noexcept;

    /// Only chunks whose archetype carries this shared value. The shared-component filter.
    void filter_shared(ComponentTypeId component, u32 value) noexcept {
        shared_filter_ = SharedValue{component, value};
    }

    /// Only chunks whose version for this component has advanced since the query last ran.
    void filter_changed(ComponentTypeId component) noexcept { change_filter_ = component; }

    template <class T>
    [[nodiscard]] Status read(const World& world) noexcept {
        return read(component_id_of<T>(world.components()));
    }
    template <class T>
    [[nodiscard]] Status write(const World& world) noexcept {
        return write(component_id_of<T>(world.components()));
    }
    template <class T>
    [[nodiscard]] Status without(const World& world) noexcept {
        return without(component_id_of<T>(world.components()));
    }

    [[nodiscard]] const ComponentMask& required() const noexcept { return required_; }
    [[nodiscard]] const ComponentMask& excluded() const noexcept { return excluded_; }
    [[nodiscard]] Span<const ComponentTypeId> required_terms() const noexcept {
        return required_terms_.span();
    }
    [[nodiscard]] Span<const ComponentTypeId> optional_terms() const noexcept {
        return optional_terms_.span();
    }
    [[nodiscard]] const jobs::AccessSet& access() const noexcept { return access_; }
    [[nodiscard]] ComponentTypeId change_filter() const noexcept { return change_filter_; }
    [[nodiscard]] const SharedValue& shared_filter() const noexcept { return shared_filter_; }

private:
    [[nodiscard]] Status require(ComponentTypeId component) noexcept;

    ComponentMask required_;
    ComponentMask excluded_;
    Array<ComponentTypeId> required_terms_;
    Array<ComponentTypeId> optional_terms_;
    jobs::AccessSet access_;
    ComponentTypeId change_filter_ = kInvalidComponent;
    SharedValue shared_filter_;
};

/// One chunk, as a query body sees it. Columns are contiguous spans: the iteration path has no
/// per-row indirection and no lookup, which is what `ecs-core`'s "iteration is contiguous" scenario
/// asks the compiler to be able to vectorise.
class QueryChunk {
public:
    QueryChunk(World& world, Archetype& archetype, u32 chunk) noexcept
        : world_(&world), archetype_(&archetype), view_(archetype.chunk(chunk)), chunk_(chunk) {}

    [[nodiscard]] u32 count() const noexcept { return view_.count(); }
    [[nodiscard]] u32 chunk_index() const noexcept { return chunk_; }
    [[nodiscard]] Archetype& archetype() const noexcept { return *archetype_; }

    [[nodiscard]] Span<const Entity> entities() const noexcept {
        return {static_cast<const Entity*>(view_.keys()), view_.count()};
    }

    [[nodiscard]] bool has(ComponentTypeId component) const noexcept {
        return archetype_->mask().test(component);
    }

    /// A column for reading. Does **not** stamp the chunk's version — that is the whole of
    /// `ecs-core`'s "read does not dirty".
    template <class T>
    [[nodiscard]] Span<const T> read(ComponentTypeId component) const noexcept {
        const i32 column = archetype_->column_of(component);
        if (column < 0) {
            return Span<const T>{};
        }
        return Span<const T>(static_cast<const T*>(view_.column(static_cast<u32>(column))),
                             view_.count());
    }

    /// A column for writing. Stamps the chunk's version for this component at the world's current
    /// version, so a downstream change filter fires for the whole chunk.
    template <class T>
    [[nodiscard]] Span<T> write(ComponentTypeId component) noexcept {
        const i32 column = archetype_->column_of(component);
        if (column < 0) {
            return Span<T>{};
        }
        view_.set_version(static_cast<u32>(column), world_->version());
        return Span<T>(static_cast<T*>(view_.column(static_cast<u32>(column))), view_.count());
    }

    /// The archetype's value for a shared component, or null when it has none.
    [[nodiscard]] const void* shared(ComponentTypeId component) const noexcept;

    /// The version this chunk carries for a component. What a change filter reads.
    [[nodiscard]] u64 version(ComponentTypeId component) const noexcept {
        const i32 column = archetype_->column_of(component);
        return (column < 0) ? 0 : view_.version(static_cast<u32>(column));
    }

private:
    World* world_;
    Archetype* archetype_;
    ChunkView view_;
    u32 chunk_;
};

/// What one iteration cost, so a diagnostic can answer "why is this query slow" with numbers.
struct QueryStats {
    u32 matched_archetypes = 0;
    u64 chunks_visited = 0;
    u64 chunks_skipped = 0;
    u64 entities_visited = 0;
};

/// A cached query. Built once, refreshed incrementally, iterated every frame.
class Query {
public:
    Query(World& world, QueryDesc&& desc) noexcept
        : world_(&world), desc_(std::move(desc)), matched_(world.allocator()) {}

    /// Bring the cached archetype list up to date. `ecs-core` requires matching to be amortised:
    /// only archetypes created since the last call are tested, so a world with a stable archetype
    /// set costs one comparison here and nothing else.
    [[nodiscard]] Status refresh() noexcept;

    [[nodiscard]] u32 matched_archetypes() const noexcept {
        return static_cast<u32>(matched_.size());
    }

    [[nodiscard]] u64 entity_count() noexcept;

    /// Iterate the matching chunks. The world is marked as iterating for the duration, so a
    /// structural change attempted from the body is refused rather than corrupting the chunk the
    /// body is walking — see world.h.
    ///
    /// `body` is called as `body(QueryChunk&)`.
    template <class Body>
    [[nodiscard]] Status for_each_chunk(Body&& body) noexcept {
        if (Status refreshed = refresh(); !refreshed) {
            return refreshed;
        }
        stats_ = QueryStats{};
        stats_.matched_archetypes = static_cast<u32>(matched_.size());

        const World::IterationGuard guard(*world_);
        const ComponentTypeId filter = desc_.change_filter();
        for (const u32 archetype_id : matched_) {
            Archetype& archetype = world_->archetypes().at(archetype_id);
            for (u32 index = 0; index < archetype.chunk_count(); ++index) {
                QueryChunk chunk(*world_, archetype, index);
                if (chunk.count() == 0) {
                    continue;
                }
                if (filter != kInvalidComponent && chunk.version(filter) <= last_run_version_) {
                    // Chunk-granular, and skipped whole. See the header.
                    ++stats_.chunks_skipped;
                    continue;
                }
                ++stats_.chunks_visited;
                stats_.entities_visited += chunk.count();
                body(chunk);
            }
        }
        last_run_version_ = world_->version();
        return ok();
    }

    /// The version this query last ran at, which is what a change filter compares against.
    [[nodiscard]] u64 last_run_version() const noexcept { return last_run_version_; }
    void set_last_run_version(u64 version) noexcept { last_run_version_ = version; }

    [[nodiscard]] const QueryDesc& desc() const noexcept { return desc_; }
    [[nodiscard]] const QueryStats& stats() const noexcept { return stats_; }

private:
    [[nodiscard]] bool matches(const Archetype& archetype) const noexcept;

    World* world_;
    QueryDesc desc_;
    Array<u32> matched_;
    /// How many archetypes have been tested. The whole of the incremental update: archetypes are
    /// never destroyed while the world lives (archetype.h), so an id once tested stays tested.
    u32 scanned_ = 0;
    u64 last_run_version_ = 0;
    QueryStats stats_;
};

}  // namespace cy::ecs
