#pragma once
// The extract stage: the ECS world becomes a render snapshot, once per commit. Task 4.1.2.
//
// `rendering-architecture` — "Simulation-to-render snapshot": rendering "SHALL consume an immutable
// render snapshot published at a defined point each frame, not live ECS storage", the snapshot is
// "extracted by an `Extract` system running at the end of the frame stage", and
//
//   "Extraction SHALL be incremental: only instances whose relevant components changed since the
//    last extraction SHALL be re-extracted, using ECS change detection."
//   "WHEN 100 000 static instances exist and 50 move THEN only the 50 changed instances SHALL be
//    re-extracted."
//
// ================================================================================================
// THE DEFINED POINT IS M2'S COMMIT BOUNDARY, AND THIS CLASS IS A `CommitObserver` FOR THAT REASON
// ================================================================================================
//
// `simulation-and-determinism` already defines one moment per tick at which state becomes
// authoritative, and its whole design argument is that "every consumer of authoritative state keys
// off it rather than defining its own moment". A renderer that sampled the world at some other
// point would be the second consumer with its own moment — the thing the boundary exists to prevent
// — so extraction is a `determinism::CommitObserver` and is CALLED rather than calling.
//
// The consequence worth stating: an extractor cannot ask when the tick committed, and cannot take
// its own copy earlier. It is handed a `CommitRecord` and the record is the only thing that says
// which tick's state it is looking at. That record's `state_version` is stamped onto the snapshot,
// so a divergence report and a rendered frame can be lined up against each other afterwards.
//
// ================================================================================================
// HOW "INCREMENTAL" IS ACTUALLY DELIVERED, AND WHERE IT IS NOT
// ================================================================================================
//
// CHANGED INSTANCES — chunk-granular change detection, on THREE components. The ECS stamps a
// chunk's version for a component when something writes that component's column (`ecs-core`: "WHEN
// a component is written THEN the whole chunk SHALL be considered changed"), so a chunk is
// re-extracted when its `WorldTransform`, its `MeshRenderer` **or** its `InterpolatedTransform`
// advanced since the previous extraction, and skipped whole otherwise.
// `ExtractStatistics::examined` against `RenderSnapshot::changed.size()` is what makes the "100 000
// static, 50 move" scenario a measurement rather than a claim.
//
// A `Query`'s own `filter_changed()` takes ONE component, so the filter here is applied in the body
// against `QueryChunk::version()` — the same numbers the built-in filter compares, tested against
// three components rather than one. The third is not decoration: `Node::teleport()` writes only the
// teleport flag, and a filter that ignored it would publish the smear the flag exists to suppress.
//
// REMOVED INSTANCES — a sweep, and only on ticks that had a structural change. The ECS has no
// destruction event log, so the honest answer is to keep the published set and check it. The check
// is skipped entirely unless `World::structural_changes()` advanced, which is what keeps a tick
// where things only moved proportional to what moved. A tick that destroyed one entity pays a scan
// of the published set; that is stated here rather than discovered from a profile, and the fix when
// it is measured is an event log in the ECS rather than a second bookkeeping scheme here.
//
// CAMERAS AND LIGHTS — carried whole, every tick. A frame has a handful of cameras and hundreds of
// lights, and the bookkeeping to diff them would cost more than the copy (snapshot.h says the same
// where the arrays are declared).
//
// ================================================================================================
// DETERMINISM
// ================================================================================================
//
// design.md §6: nothing ordered may come from iteration order over a hash map, a pointer value, or
// publication order. The snapshot's arrays are filled in query iteration order — archetype id, then
// chunk, then row — which is a function of the world's construction and identical across two runs
// of the same simulation. The published set is a SORTED ARRAY of stable ids rather than a hash map,
// so the removal list is in stable-id order by construction rather than by luck.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/commit.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/query.h>
#include <cy/ecs/world.h>
#include <cy/rendering/scene/components.h>
#include <cy/scene/components.h>
#include <cy/servers/render/snapshot.h>

namespace cy::rendering {

/// What one extraction did. Every field is a count rather than a duration: the durations belong to
/// the frame's stage timers (`render::FrameStatistics`), and what this reports is the WORK, which
/// is what the incremental requirement is about.
struct ExtractStatistics {
    /// Entities in chunks the change filter kept. The denominator of "only the 50 changed
    /// instances were re-extracted".
    u32 examined = 0;
    u32 published = 0;
    u32 removed = 0;
    u32 cameras = 0;
    u32 lights = 0;
    u32 chunks_visited = 0;
    u32 chunks_skipped = 0;
    /// Whether the removal sweep ran this tick. False on a tick with no structural change, which is
    /// the common one.
    bool swept = false;
};

/// Extraction, as a commit observer.
///
/// NOT THREAD-SAFE and called from one place: `CommitBoundary::commit()`, on the tick thread, after
/// the state version has incremented. It writes the snapshot buffer's BACK buffer and publishes it,
/// so the render thread reading the front buffer is never looking at what this is writing — the
/// whole of the synchronisation argument, and the reason there are two buffers rather than a queue.
class SnapshotExtractor final : public determinism::CommitObserver {
public:
    SnapshotExtractor(Allocator& allocator, World& world, const RenderComponents& render_components,
                      const scene::SceneComponents& scene_components,
                      render::SnapshotBuffer& buffer) noexcept;

    SnapshotExtractor(const SnapshotExtractor&) = delete;
    SnapshotExtractor& operator=(const SnapshotExtractor&) = delete;

    [[nodiscard]] const char* name() const noexcept override { return "render.extract"; }

    /// Extract and publish. `record` is the tick that just committed.
    [[nodiscard]] Status on_commit(const determinism::CommitRecord& record) noexcept override;

    /// The environment the snapshot carries. Set by whoever owns the scene's environment — a level,
    /// a weather system, an editor — and copied into every snapshot until it is set again. Held
    /// here rather than read from a component because an environment is per scene and not per
    /// entity, and a scene-level singleton read through an entity would be a component with exactly
    /// one instance.
    void set_environment(const render::EnvironmentSettings& environment) noexcept {
        environment_ = environment;
    }
    [[nodiscard]] const render::EnvironmentSettings& environment() const noexcept {
        return environment_;
    }

    [[nodiscard]] const ExtractStatistics& statistics() const noexcept { return stats_; }

    /// Stable ids currently published, in ascending order. For a test and for a diagnostic; a frame
    /// never reads it.
    [[nodiscard]] Span<const u64> published() const noexcept { return published_.span(); }

    /// Forget everything published, so the next extraction re-publishes the world.
    ///
    /// What a level unload or a scene swap calls: the next snapshot then carries every instance as
    /// changed rather than as a diff against a world that no longer exists. It does NOT emit
    /// removals — the consumer is being torn down too, and a removal list naming instances nobody
    /// holds is work for no reader.
    void reset() noexcept;

private:
    [[nodiscard]] Status extract_instances(render::RenderSnapshot& snapshot) noexcept;
    /// One chunk's rows, published. Separate from the loop above it so that the chunk-level
    /// decision (does this chunk need re-reading) and the row-level work (build a record, remember
    /// its identity) are two things a reader can hold one at a time.
    [[nodiscard]] Status publish_chunk(ecs::QueryChunk& chunk,
                                       render::RenderSnapshot& snapshot) noexcept;
    [[nodiscard]] Status sweep_removed(render::RenderSnapshot& snapshot) noexcept;
    [[nodiscard]] Status extract_cameras(render::RenderSnapshot& snapshot) noexcept;
    [[nodiscard]] Status extract_lights(render::RenderSnapshot& snapshot) noexcept;

    /// Insert into the sorted published set. Returns false only on an allocation failure.
    [[nodiscard]] bool remember(u64 stable_id) noexcept;

    Allocator* allocator_;
    World* world_;
    RenderComponents render_;
    scene::SceneComponents scene_;
    render::SnapshotBuffer* buffer_;

    /// Stable ids of everything currently in the consumer's scene, ascending. An array and not a
    /// hash map: see the header's determinism note.
    Array<u64> published_;

    /// The world version the previous extraction ran at. The baseline the chunk change filter
    /// compares against — held here rather than in a cached `Query` because the filter tests two
    /// components and the query is rebuilt per extraction (a handful of archetypes; the cached form
    /// is the upgrade when a profile asks for it).
    u64 last_version_ = 0;
    /// `World::structural_changes()` at the previous extraction. When it has not moved, nothing was
    /// created or destroyed and the removal sweep is skipped.
    u64 last_structural_ = 0;
    bool extracted_once_ = false;

    render::EnvironmentSettings environment_;
    ExtractStatistics stats_;
};

}  // namespace cy::rendering
